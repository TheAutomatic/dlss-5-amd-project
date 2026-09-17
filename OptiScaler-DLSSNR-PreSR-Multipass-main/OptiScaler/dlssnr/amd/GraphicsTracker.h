#pragma once
#include "GraphicsSnapshot.h"
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

// CPU observations and freezes are serialized. No mutable state pointer escapes
// the mutex: lists may be reset/released concurrently, and OM owns descriptors.
namespace AmdPreSr::GraphicsSnap
{
inline thread_local bool g_restoreArmed = false;
inline bool RestoreArmed() { return g_restoreArmed; }

class Tracker
{
  public:
    void SetEnabled(bool on)
    {
        std::unique_lock lock(mutex_);
        if (enabled_ != on)
            trackers_.clear(); // An unobserved interval invalidates generations.
        enabled_ = on;
    }

    bool IsEnabled() const
    {
        std::shared_lock lock(mutex_);
        return enabled_;
    }

    void OnCreate(uint64_t listId, uint64_t initialPso = 0)
    {
        std::unique_lock lock(mutex_);
        if (!enabled_)
            return;
        trackers_[listId].OnCreate(listId, initialPso);
        suppress_.erase(listId);
    }

    // Successful Reset establishes a fully observed generation even when Create
    // preceded the hooks. Failed Reset changes nothing, including absent records.
    bool OnReset(uint64_t listId, bool succeeded, uint64_t initialPso = 0)
    {
        std::unique_lock lock(mutex_);
        if (!enabled_ || !succeeded)
            return false;
        auto it = trackers_.find(listId);
        if (it == trackers_.end())
        {
            trackers_[listId].OnCreate(listId, initialPso);
            return true;
        }
        return it->second.OnReset(true, initialPso);
    }

    void OnClearState(uint64_t listId, uint64_t initialPso = 0)
    {
        Mutate(listId, false, [&](ListTracker& t) { t.OnClearState(initialPso); });
    }

    void OnRelease(uint64_t listId)
    {
        std::unique_lock lock(mutex_);
        trackers_.erase(listId);
        suppress_.erase(listId);
    }

    bool PushSuppress(uint64_t listId)
    {
        std::unique_lock lock(mutex_);
        if (!enabled_)
            return false;
        ++suppress_[listId];
        return true;
    }

    void PopSuppress(uint64_t listId)
    {
        std::unique_lock lock(mutex_);
        if (auto it = suppress_.find(listId); it != suppress_.end() && --it->second == 0)
            suppress_.erase(it);
    }

    void ReportGraphicsRootSignature(uint64_t listId, uint64_t sig, bool fromRestore = false)
    { Mutate(listId, fromRestore, [&](ListTracker& t) { t.snap.graphics.SetSignature(sig); }); }
    void ReportComputeRootSignature(uint64_t listId, uint64_t sig, bool fromRestore = false)
    { Mutate(listId, fromRestore, [&](ListTracker& t) { t.snap.compute.SetSignature(sig); }); }
    void ReportRootTable(uint64_t listId, bool graphics, uint32_t index, uint64_t handle,
                         bool fromRestore = false)
    { Mutate(listId, fromRestore, [&](ListTracker& t) { Domain(t, graphics).SetTable(index, handle); }); }
    void ReportRootConstants(uint64_t listId, bool graphics, uint32_t index, const uint32_t* src,
                             uint32_t count, uint32_t destOffset, bool fromRestore = false)
    {
        Mutate(listId, fromRestore, [&](ListTracker& t) {
            if (!Domain(t, graphics).MergeConstants(index, src, count, destOffset))
                t.MarkIneligible();
        });
    }
    void ReportRootConstant(uint64_t listId, bool graphics, uint32_t index, uint32_t value,
                            uint32_t destOffset, bool fromRestore = false)
    { ReportRootConstants(listId, graphics, index, &value, 1, destOffset, fromRestore); }
    void ReportRootGpuVa(uint64_t listId, bool graphics, uint32_t index, RootEntryType type,
                         uint64_t va, bool fromRestore = false)
    { Mutate(listId, fromRestore, [&](ListTracker& t) { Domain(t, graphics).SetGpuVa(index, type, va); }); }

    // Known ExecuteIndirect signatures reset only their named root arguments.
    void ReportIndirectRootConstants(uint64_t listId, bool graphics, uint32_t index,
                                     uint32_t destOffset, uint32_t count)
    {
        const uint32_t zeros[kMaxRootConstants] {};
        if (count > kMaxRootConstants)
            MarkIneligible(listId, IneligibleWhy::Indirect);
        else
            ReportRootConstants(listId, graphics, index, zeros, count, destOffset);
    }
    void ReportIndirectRootGpuVa(uint64_t listId, bool graphics, uint32_t index, RootEntryType type)
    { ReportRootGpuVa(listId, graphics, index, type, 0); }

    void ReportHeaps(uint64_t listId, uint32_t count, const uint64_t* handles, bool fromRestore = false)
    { Mutate(listId, fromRestore, [&](ListTracker& t) { t.snap.SetHeaps(count, handles); }); }
    void ReportPso(uint64_t listId, uint64_t pso, bool fromRestore = false)
    { Mutate(listId, fromRestore, [&](ListTracker& t) { t.snap.SetPso(pso); }); }
    void ReportViewports(uint64_t listId, const Viewport* v, uint32_t count, bool fromRestore = false)
    { Mutate(listId, fromRestore, [&](ListTracker& t) { t.snap.SetViewports(v, count); }); }
    void ReportScissors(uint64_t listId, const ScissorRect* r, uint32_t count, bool fromRestore = false)
    { Mutate(listId, fromRestore, [&](ListTracker& t) { t.snap.SetScissors(r, count); }); }
    void ReportTopology(uint64_t listId, uint32_t topology, bool fromRestore = false)
    { Mutate(listId, fromRestore, [&](ListTracker& t) { t.snap.SetTopology(topology); }); }
    void ReportRenderTargets(uint64_t listId, uint32_t numRTVs, const uint64_t* rtvHandles,
                             bool singleHandleRange, bool hasDsv, uint64_t dsvHandle,
                             bool fromRestore = false, std::shared_ptr<void> owner = {})
    {
        Mutate(listId, fromRestore, [&](ListTracker& t) {
            t.snap.SetRenderTargets(numRTVs, rtvHandles, singleHandleRange, hasDsv, dsvHandle, std::move(owner));
        });
    }
    void ReportOmUnknown(uint64_t listId, bool fromRestore = false)
    { Mutate(listId, fromRestore, [](ListTracker& t) { t.snap.om = OmBinding {}; }); }
    void ReportPredication(uint64_t listId, uint64_t resource, uint64_t byteOffset, uint32_t operation,
                           bool fromRestore = false)
    { Mutate(listId, fromRestore, [&](ListTracker& t) { t.snap.SetPredication(resource, byteOffset, operation); }); }
    void MarkIneligible(uint64_t listId, IneligibleWhy why = IneligibleWhy::Other)
    { Mutate(listId, false, [&](ListTracker& t) { t.MarkIneligible(why); }); }

    void OnBeginQuery(uint64_t listId, uint64_t heap, uint32_t type, uint32_t index)
    {
        Mutate(listId, false, [&](ListTracker& t) {
            t.activeQueries.emplace(heap, type, index);
            t.snap.queryActive = true;
        });
    }
    void OnEndQuery(uint64_t listId, uint64_t heap, uint32_t type, uint32_t index, bool timestamp = false)
    {
        if (timestamp)
            return; // TIMESTAMP has no BeginQuery and cannot close another query.
        Mutate(listId, false, [&](ListTracker& t) {
            t.activeQueries.erase({ heap, type, index });
            t.snap.queryActive = !t.activeQueries.empty();
        });
    }

    void OnBeginRenderPass(uint64_t listId, bool suspending = false, bool resuming = false)
    {
        Mutate(listId, false, [&](ListTracker& t) {
            (void)resuming; // Resume may originate from a different command list.
            t.snap.renderPassActive = true;
            t.snap.renderPassSuspended = false;
            t.renderPassWillSuspend = suspending;
            t.snap.om = OmBinding {};
        });
    }
    void OnEndRenderPass(uint64_t listId)
    {
        Mutate(listId, false, [](ListTracker& t) {
            t.snap.renderPassActive = false;
            t.snap.renderPassSuspended = t.renderPassWillSuspend;
            t.renderPassWillSuspend = false;
            // BeginRenderPass sets its own OM. A subsequent explicit OM bind is
            // needed to reconstruct the legacy state without guessing.
            t.snap.om = OmBinding {};
        });
    }
    bool IsRenderPassUnsafe(uint64_t listId) const
    {
        std::shared_lock lock(mutex_);
        auto it = trackers_.find(listId);
        return enabled_ && it != trackers_.end() &&
               (it->second.snap.renderPassActive || it->second.snap.renderPassSuspended);
    }

    bool TryFreeze(uint64_t listId, GraphicsSnapshot& out) const
    {
        std::shared_lock lock(mutex_);
        auto it = trackers_.find(listId);
        return enabled_ && it != trackers_.end() && ::AmdPreSr::GraphicsSnap::TryFreeze(it->second, out);
    }
    bool CopyState(uint64_t listId, GraphicsSnapshot& out, uint32_t& generation, bool& generationKnown) const
    {
        std::shared_lock lock(mutex_);
        auto it = trackers_.find(listId);
        if (!enabled_ || it == trackers_.end())
            return false;
        out = it->second.snap;
        generation = it->second.generation;
        generationKnown = it->second.generationKnown;
        return true;
    }
    bool CanAdmit(uint64_t listId) const { return std::strcmp(AdmitReason(listId), "ok") == 0; }
    const char* AdmitReason(uint64_t listId) const
    {
        std::shared_lock lock(mutex_);
        if (!enabled_)
            return "tracker_off";
        auto it = trackers_.find(listId);
        return it == trackers_.end() ? "no_record" : CanAdmitGraphics(it->second).reason;
    }
    bool HasRecord(uint64_t listId) const
    {
        std::shared_lock lock(mutex_);
        return trackers_.contains(listId);
    }

  private:
    static RootDomain& Domain(ListTracker& t, bool graphics) { return graphics ? t.snap.graphics : t.snap.compute; }
    template<class Fn> void Mutate(uint64_t listId, bool fromRestore, Fn&& fn)
    {
        std::unique_lock lock(mutex_);
        if (!enabled_ || (!fromRestore && suppress_.contains(listId)))
            return;
        auto& t = trackers_[listId];
        if (!t.live)
        {
            t.listId = listId;
            t.live = true;
            t.generationKnown = false; // No successful Create/Reset observed.
        }
        fn(t); // Mutation and descriptor-owner replacement stay under the lock.
    }
    mutable std::shared_mutex mutex_;
    bool enabled_ = false;
    std::unordered_map<uint64_t, ListTracker> trackers_;
    std::unordered_map<uint64_t, uint32_t> suppress_;
};

inline Tracker& GraphicsTracker()
{
    static Tracker tracker;
    return tracker;
}

// List-specific nesting suppresses temporary SR/NR state. Explicit trampoline
// restore reports use fromRestore=true and still update the final tracked state.
class ScopedCaptureSuppression
{
  public:
    explicit ScopedCaptureSuppression(uint64_t listId, bool enabled = true)
        : ScopedCaptureSuppression(GraphicsTracker(), listId, enabled) {}
    ScopedCaptureSuppression(Tracker& tracker, uint64_t listId, bool enabled = true)
        : tracker_(tracker), listId_(listId), pushed_(enabled && tracker.PushSuppress(listId)) {}
    ~ScopedCaptureSuppression() { if (pushed_) tracker_.PopSuppress(listId_); }
    ScopedCaptureSuppression(const ScopedCaptureSuppression&) = delete;
    ScopedCaptureSuppression& operator=(const ScopedCaptureSuppression&) = delete;
  private:
    Tracker& tracker_;
    uint64_t listId_;
    bool pushed_;
};
} // namespace AmdPreSr::GraphicsSnap
