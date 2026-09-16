#pragma once
#include "GraphicsSnapshot.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>

// Adapter that feeds GraphicsSnapshot from D3D12 Set* observations.
// Off unless AmdGraphicsWait=1 so the r26/r27 compute path is unchanged.
// Pure CPU: host-contract tests drive this without D3D12.
namespace AmdPreSr::GraphicsSnap
{

class Tracker
{
  public:
    void SetEnabled(bool on)
    {
        std::unique_lock lock(mutex_);
        enabled_ = on;
    }

    bool IsEnabled() const
    {
        std::shared_lock lock(mutex_);
        return enabled_;
    }

    void OnCreate(uint64_t listId)
    {
        std::unique_lock lock(mutex_);
        if (!enabled_)
            return;
        auto& t = trackers_[listId];
        t.OnCreate(listId);
        suppress_[listId] = 0;
    }

    // Late Reset without a seen Create starts an unknown-generation record.
    bool OnReset(uint64_t listId, bool succeeded)
    {
        std::unique_lock lock(mutex_);
        if (!enabled_)
            return false;
        auto it = trackers_.find(listId);
        if (it == trackers_.end())
        {
            auto& t = trackers_[listId];
            t.listId = listId;
            t.live = true;
            t.generation = 1;
            t.generationKnown = false;
            t.snap = GraphicsSnapshot {};
            return true;
        }
        return it->second.OnReset(succeeded);
    }

    void OnClearState(uint64_t listId)
    {
        std::unique_lock lock(mutex_);
        if (!enabled_)
            return;
        if (auto it = trackers_.find(listId); it != trackers_.end())
            it->second.OnClearState();
    }

    void OnRelease(uint64_t listId)
    {
        std::unique_lock lock(mutex_);
        if (!enabled_)
            return;
        if (auto it = trackers_.find(listId); it != trackers_.end())
            it->second.OnRelease();
        trackers_.erase(listId);
        suppress_.erase(listId);
    }

    void PushSuppress(uint64_t listId)
    {
        std::unique_lock lock(mutex_);
        if (!enabled_)
            return;
        ++suppress_[listId];
    }

    void PopSuppress(uint64_t listId)
    {
        std::unique_lock lock(mutex_);
        if (!enabled_)
            return;
        if (auto it = suppress_.find(listId); it != suppress_.end() && it->second > 0)
            --it->second;
    }

    // fromRestore=true bypasses suppression: RestoreRoot trampoline direct-calls
    // must update the tracker even while the observer path is suppressed.
    void ReportGraphicsRootSignature(uint64_t listId, uint64_t sig, bool fromRestore = false)
    {
        ReportSig(listId, true, sig, fromRestore);
    }

    void ReportComputeRootSignature(uint64_t listId, uint64_t sig, bool fromRestore = false)
    {
        ReportSig(listId, false, sig, fromRestore);
    }

    void ReportRootTable(uint64_t listId, bool graphics, uint32_t index, uint64_t handle,
                         bool fromRestore = false)
    {
        auto* d = Acquire(listId, fromRestore, graphics);
        if (!d)
            return;
        d->SetTable(index, handle);
    }

    void ReportRootConstants(uint64_t listId, bool graphics, uint32_t index, const uint32_t* src,
                             uint32_t count, uint32_t destOffset, bool fromRestore = false)
    {
        auto* d = Acquire(listId, fromRestore, graphics);
        if (!d)
            return;
        d->MergeConstants(index, src, count, destOffset);
    }

    void ReportRootConstant(uint64_t listId, bool graphics, uint32_t index, uint32_t value,
                            uint32_t destOffset, bool fromRestore = false)
    {
        ReportRootConstants(listId, graphics, index, &value, 1, destOffset, fromRestore);
    }

    void ReportRootGpuVa(uint64_t listId, bool graphics, uint32_t index, RootEntryType type,
                         uint64_t va, bool fromRestore = false)
    {
        auto* d = Acquire(listId, fromRestore, graphics);
        if (!d)
            return;
        d->SetGpuVa(index, type, va);
    }

    void ReportPso(uint64_t listId, uint64_t pso, bool fromRestore = false)
    {
        auto* s = AcquireSnap(listId, fromRestore);
        if (!s)
            return;
        s->SetPso(pso);
    }

    void ReportViewports(uint64_t listId, const Viewport* v, uint32_t count, bool fromRestore = false)
    {
        auto* s = AcquireSnap(listId, fromRestore);
        if (!s)
            return;
        s->SetViewports(v, count);
    }

    void ReportScissors(uint64_t listId, const ScissorRect* r, uint32_t count, bool fromRestore = false)
    {
        auto* s = AcquireSnap(listId, fromRestore);
        if (!s)
            return;
        s->SetScissors(r, count);
    }

    void ReportTopology(uint64_t listId, uint32_t topology, bool fromRestore = false)
    {
        auto* s = AcquireSnap(listId, fromRestore);
        if (!s)
            return;
        s->SetTopology(topology);
    }

    void ReportRenderTargets(uint64_t listId, uint32_t numRTVs, const uint64_t* rtvHandles,
                             bool singleHandleRange, bool hasDsv, uint64_t dsvHandle,
                             bool fromRestore = false)
    {
        auto* s = AcquireSnap(listId, fromRestore);
        if (!s)
            return;
        s->SetRenderTargets(numRTVs, rtvHandles, singleHandleRange, hasDsv, dsvHandle);
    }

    void ReportPredication(uint64_t listId, uint64_t resource, uint64_t byteOffset, uint32_t operation,
                           bool fromRestore = false)
    {
        auto* s = AcquireSnap(listId, fromRestore);
        if (!s)
            return;
        s->SetPredication(resource, byteOffset, operation);
    }

    void MarkIneligible(uint64_t listId)
    {
        std::unique_lock lock(mutex_);
        if (!enabled_)
            return;
        if (auto it = trackers_.find(listId); it != trackers_.end())
            it->second.MarkIneligible();
    }

    bool TryFreeze(uint64_t listId, GraphicsSnapshot& out) const
    {
        std::shared_lock lock(mutex_);
        if (!enabled_)
            return false;
        auto it = trackers_.find(listId);
        if (it == trackers_.end())
            return false;
        return ::AmdPreSr::GraphicsSnap::TryFreeze(it->second, out);
    }

    bool CanAdmit(uint64_t listId) const
    {
        auto* reason = AdmitReason(listId);
        return reason && reason[0] == 'o' && reason[1] == 'k' && reason[2] == '\0';
    }

    // Stable reason token for logs; first string is from CanAdmitGraphics.
    const char* AdmitReason(uint64_t listId) const
    {
        std::shared_lock lock(mutex_);
        if (!enabled_)
            return "tracker_off";
        auto it = trackers_.find(listId);
        if (it == trackers_.end())
            return "no_record";
        return CanAdmitGraphics(it->second).reason;
    }

    // True when this list has a live record (generation may still be unknown).
    bool HasRecord(uint64_t listId) const
    {
        std::shared_lock lock(mutex_);
        return trackers_.contains(listId);
    }

  private:
    bool IsSuppressed(uint64_t listId) const
    {
        auto it = suppress_.find(listId);
        return it != suppress_.end() && it->second > 0;
    }

    ListTracker* AcquireTracker(uint64_t listId)
    {
        auto it = trackers_.find(listId);
        if (it == trackers_.end())
        {
            // First observation without Create/Reset: unknown generation.
            auto& t = trackers_[listId];
            t.listId = listId;
            t.live = true;
            t.generationKnown = false;
            return &t;
        }
        if (!it->second.live)
            return nullptr;
        return &it->second;
    }

    // Returns the root domain, or nullptr when disabled/suppressed.
    RootDomain* Acquire(uint64_t listId, bool fromRestore, bool graphics)
    {
        std::unique_lock lock(mutex_);
        if (!enabled_)
            return nullptr;
        if (!fromRestore && IsSuppressed(listId))
            return nullptr;
        auto* t = AcquireTracker(listId);
        if (!t)
            return nullptr;
        return graphics ? &t->snap.graphics : &t->snap.compute;
    }

    GraphicsSnapshot* AcquireSnap(uint64_t listId, bool fromRestore)
    {
        std::unique_lock lock(mutex_);
        if (!enabled_)
            return nullptr;
        if (!fromRestore && IsSuppressed(listId))
            return nullptr;
        auto* t = AcquireTracker(listId);
        if (!t)
            return nullptr;
        return &t->snap;
    }

    void ReportSig(uint64_t listId, bool graphics, uint64_t sig, bool fromRestore)
    {
        auto* d = Acquire(listId, fromRestore, graphics);
        if (!d)
            return;
        d->SetSignature(sig);
    }

    mutable std::shared_mutex mutex_;
    bool enabled_ = false;
    std::unordered_map<uint64_t, ListTracker> trackers_;
    std::unordered_map<uint64_t, int> suppress_;
};

inline Tracker& GraphicsTracker()
{
    static Tracker tracker;
    return tracker;
}

} // namespace AmdPreSr::GraphicsSnap
