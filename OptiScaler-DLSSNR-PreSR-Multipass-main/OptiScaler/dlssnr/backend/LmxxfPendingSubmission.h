#pragma once
#include <d3d12.h>

namespace DlssNr::Backend
{
// Independent of the between-slot payload: HIP enqueue consumes that payload
// before ExecuteExpanded submits the continuation that references this job.
// Caller holds the backend job mutex and lifecycle lock.
struct LmxxfPendingSubmission
{
    void* job = nullptr;
    ID3D12CommandList* cmd = nullptr;

    bool Contains(UINT count, ID3D12CommandList* const* lists) const
    {
        if (!cmd || !lists) return false;
        for (UINT i = 0; i < count; ++i)
            if (lists[i] == cmd) return true;
        return false;
    }

    void* TakeSubmitted(UINT count, ID3D12CommandList* const* lists)
    {
        if (!Contains(count, lists)) return nullptr;
        void* submitted = job;
        *this = {};
        return submitted;
    }
};
} // namespace DlssNr::Backend
