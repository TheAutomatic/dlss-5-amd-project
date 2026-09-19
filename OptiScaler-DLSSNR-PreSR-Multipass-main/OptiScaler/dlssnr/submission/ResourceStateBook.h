#pragma once
#include <d3d12.h>
#include <cstdint>
#include <unordered_map>

namespace DlssNr::Submission
{
// Tracks barriers seen on the logical list for split admission.
// Not a full D3D12 state validator — fail-closed on known-dangerous patterns.
class ResourceStateBook
{
    // Last known state after barriers on this logical recording (producer side).
    std::unordered_map<ID3D12Resource *, D3D12_RESOURCE_STATES> states;
    bool openSplitBarrier = false;
    bool sawAliasing = false;
    bool sawUnorderedAccess = false;

  public:
    void Reset()
    {
        states.clear();
        openSplitBarrier = false;
        sawAliasing = false;
        sawUnorderedAccess = false;
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
                sawAliasing = true;
                if (reasonOut)
                    *reasonOut = "aliasing_barrier";
                return false;
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
        if (sawAliasing)
        {
            if (reasonOut)
                *reasonOut = "aliasing_barrier";
            return false;
        }
        return true;
    }

    size_t TrackedCount() const { return states.size(); }
};
} // namespace DlssNr::Submission
