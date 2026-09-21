#pragma once
#include "../submission/CommandListProxy.h"
#include "Selector.h"
#include "../submission/SubmissionHooks.h"
#include <atomic>
#include <cstdint>

// Evaluate-time cut for lmxxf: Split the recording proxy, then HIP in the Execute between slot.
// Product call site is gated by SubmissionHooksWanted() (LmxxfWired() && NrBackend=lmxxf).
// AmdBridge calls this every Evaluate; no-op unless SubmissionHooksWanted(). Harnesses can call helpers directly.
namespace DlssNr::Backend::LmxxfCut
{
using EnqueueHipFn = int32_t (*)(void *session, void *job);

// lastEnqueueRc when BetweenThunk ran but Pending was empty (HIP skipped).
constexpr int32_t kEnqueueSkipped = static_cast<int32_t>(0x534B4950); // 'SKIP'

struct PendingHip
{
    void *session = nullptr;
    void *job = nullptr;
    EnqueueHipFn enqueueHip = nullptr;
    std::atomic<int> betweenHits { 0 };
    std::atomic<int> enqueueCalls { 0 };
    std::atomic<int> skippedHits { 0 };
    std::atomic<int32_t> lastEnqueueRc { 0 };
};

inline PendingHip &Pending()
{
    // Boundary: process-wide singleton. First product version is one NR session;
    // multi-feature / multi-context must not share this without a map keyed by session.
    static PendingHip p;
    return p;
}

inline void ClearPendingEnqueue()
{
    auto &p = Pending();
    p.session = nullptr;
    p.job = nullptr;
    p.enqueueHip = nullptr;
}

inline void BetweenThunk(ID3D12CommandQueue *queue, void * /*ctx*/)
{
    auto &p = Pending();
    if (!(p.enqueueHip && p.session && p.job))
    {
        p.skippedHits.fetch_add(1, std::memory_order_relaxed);
        p.lastEnqueueRc.store(kEnqueueSkipped, std::memory_order_relaxed);
        return;
    }
    auto *const session = p.session;
    auto *const job = p.job;
    const EnqueueHipFn fn = p.enqueueHip;
    // Consume before call so a nested Submitted cannot double-fire the same job.
    ClearPendingEnqueue();
    p.betweenHits.fetch_add(1, std::memory_order_relaxed);
    p.enqueueCalls.fetch_add(1, std::memory_order_relaxed);
    p.lastEnqueueRc.store(fn(session, job), std::memory_order_relaxed);
}

// QI for ILogicalCommandList and SplitSegments. S_FALSE = not our proxy (cannot sandwich).
inline HRESULT TrySplitAtEvaluate(ID3D12GraphicsCommandList *cmd)
{
    if (!cmd)
        return E_INVALIDARG;
    DlssNr::Submission::ILogicalCommandList *logical = nullptr;
    if (FAILED(cmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList), reinterpret_cast<void **>(&logical))) || !logical)
        return S_FALSE;
    const HRESULT hr = logical->SplitSegments();
    logical->Release();
    return hr;
}

inline void SetPendingEnqueue(void *session, void *job, EnqueueHipFn enqueueHip)
{
    auto &p = Pending();
    p.session = session;
    p.job = job;
    p.enqueueHip = enqueueHip;
}

inline void ArmBetweenSlot() { DlssNr::Submission::Hooks::SetBetween(&BetweenThunk, nullptr); }

inline void DisarmBetweenSlot()
{
    DlssNr::Submission::Hooks::SetBetween(nullptr, nullptr);
    ClearPendingEnqueue();
}

// Product Evaluate/Before hook. Split + SetPendingEnqueue live in LmxxfBackend::Record
// (after RecordInputs). Kept as a no-op gate so call sites stay stable.
inline void OnEvaluateBeforeRecord(ID3D12GraphicsCommandList * /*cmd*/)
{
    if (!DlssNr::Backend::SubmissionHooksWanted())
        return;
}
} // namespace DlssNr::Backend::LmxxfCut
