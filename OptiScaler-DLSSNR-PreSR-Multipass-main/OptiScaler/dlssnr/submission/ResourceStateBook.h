#pragma once
#include <d3d12.h>
#include <cstdint>
#include <unordered_map>

namespace DlssNr::Submission
{
// Tracks barriers seen on the logical list for split admission and
// post-Execute promotion/decay expectations (plan D / M3).
// Not a full D3D12 state validator — fail-closed on known-dangerous patterns.
class ResourceStateBook
{
    // Last known state after barriers on this logical recording.
    std::unordered_map<ID3D12Resource *, D3D12_RESOURCE_STATES> states;
    bool openSplitBarrier = false;
    bool sawUnorderedAccess = false;

  public:
    void Reset()
    {
        states.clear();
        openSplitBarrier = false;
        sawUnorderedAccess = false;
    }

    // States that do NOT decay across ExecuteCommandLists (Microsoft M3).
    static bool SurvivesExecute(D3D12_RESOURCE_STATES s)
    {
        const D3D12_RESOURCE_STATES keep =
            D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_RENDER_TARGET |
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_COPY_DEST |
            D3D12_RESOURCE_STATE_RESOLVE_DEST | D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE |
            D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE | D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE |
            D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE | D3D12_RESOURCE_STATE_STREAM_OUT;
        return (s & keep) != 0;
    }

    // After producer Execute, decay tracked states to the expected post-Execute
    // values so continuation-side tracking starts from the boundary, not a wipe.
    void ApplyExecuteDecay()
    {
        for (auto &kv : states)
        {
            if (!SurvivesExecute(kv.second))
                kv.second = D3D12_RESOURCE_STATE_COMMON;
        }
        openSplitBarrier = false;
        // UAV flag is informational; leave sawUnorderedAccess as-is for diagnostics.
    }

    bool TryGet(ID3D12Resource *r, D3D12_RESOURCE_STATES *out) const
    {
        if (!r || !out)
            return false;
        auto it = states.find(r);
        if (it == states.end())
            return false;
        *out = it->second;
        return true;
    }

    // Returns false + reason if this barrier batch makes a later Split unsafe.
    bool OnBarriers(UINT n, const D3D12_RESOURCE_BARRIER *b, const char **reasonOut)
    {
        if (reasonOut)
            *reasonOut = nullptr;
        if (!b || n == 0)
            return true;
        for (UINT i = 0; i < n; ++i)
        {
            const D3D12_RESOURCE_BARRIER &bar = b[i];
            if (bar.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING)
            {
                // The barrier is already recorded in the producer before a cut. The
                // continuation executes later on the same queue, preserving its order.
                // Aliased resources no longer have reliable entries in this state book.
                if (!bar.Aliasing.pResourceBefore || !bar.Aliasing.pResourceAfter)
                    states.clear();
                else
                {
                    states.erase(bar.Aliasing.pResourceBefore);
                    states.erase(bar.Aliasing.pResourceAfter);
                }
                continue;
            }
            if (bar.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV)
            {
                sawUnorderedAccess = true;
                // UAV barrier alone is OK to track; do not reject yet.
                continue;
            }
            if (bar.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
                continue;
            const auto flags = bar.Flags;
            if (flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)
                openSplitBarrier = true;
            if (flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY)
            {
                if (!openSplitBarrier)
                {
                    if (reasonOut)
                        *reasonOut = "split_barrier_end_without_begin";
                    return false;
                }
                openSplitBarrier = false;
            }
            if (bar.Transition.pResource)
                states[bar.Transition.pResource] = bar.Transition.StateAfter;
        }
        return true;
    }

    // Call before Split: open BEGIN_ONLY without END must refuse the cut.
    bool CanSplit(const char **reasonOut) const
    {
        if (reasonOut)
            *reasonOut = nullptr;
        if (openSplitBarrier)
        {
            if (reasonOut)
                *reasonOut = "open_split_barrier";
            return false;
        }
        return true;
    }

    size_t TrackedCount() const { return states.size(); }
};
} // namespace DlssNr::Submission
