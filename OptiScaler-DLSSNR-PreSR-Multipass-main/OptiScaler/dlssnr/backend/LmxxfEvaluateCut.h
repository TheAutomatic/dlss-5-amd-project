#pragma once
#include "../submission/CommandListProxy.h"
#include "Selector.h"
#include "../submission/SubmissionHooks.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

// Evaluate-time cut for lmxxf: Split the recording proxy, then HIP in the Execute between slot.
// Product call site is gated by SubmissionHooksWanted() (LmxxfWired() && NrBackend=lmxxf).
// AmdBridge calls this every Evaluate; no-op unless SubmissionHooksWanted(). Harnesses can call helpers directly.
namespace DlssNr::Backend::LmxxfCut
{
using EnqueueHipFn = int32_t (*)(void *session, void *job, void *command_queue);
using GetLastErrorFn = int32_t (*)(char *buffer, uint32_t buffer_chars);

// lastEnqueueRc when BetweenThunk ran but Pending was empty (HIP skipped).
constexpr int32_t kEnqueueSkipped = static_cast<int32_t>(0x534B4950); // 'SKIP'
constexpr int32_t kEnqueueQueueMismatch = static_cast<int32_t>(0x514D4953); // 'QMIS'

struct PendingHip
{
    std::mutex mutex;
    void *session = nullptr;
    void *job = nullptr;
    EnqueueHipFn enqueueHip = nullptr;
    GetLastErrorFn getLastError = nullptr;
    ID3D12CommandList *targetList = nullptr; // Identity only; the backend owns the pending job.
    ID3D12CommandQueue *expectedQueue = nullptr;
    std::array<char, 256> lastEnqueueError {};
    ID3D12CommandQueue *lastEnqueueQueue = nullptr;
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
    std::lock_guard lock(p.mutex);
    p.session = nullptr;
    p.job = nullptr;
    p.enqueueHip = nullptr;
    p.getLastError = nullptr;
    p.targetList = nullptr;
    p.expectedQueue = nullptr;
}

inline void ClearPendingEnqueueIfSubmitted(UINT count, ID3D12CommandList *const *lists)
{
    if (!lists)
        return;
    auto &p = Pending();
    std::lock_guard lock(p.mutex);
    for (UINT i = 0; i < count; ++i)
    {
        if (p.targetList && lists[i] == p.targetList)
        {
            p.session = nullptr;
            p.job = nullptr;
            p.enqueueHip = nullptr;
            p.getLastError = nullptr;
            p.targetList = nullptr;
            p.expectedQueue = nullptr;
            return;
        }
    }
}

inline void BetweenThunk(ID3D12CommandQueue *queue, ID3D12CommandList *list, void * /*ctx*/)
{
    auto &p = Pending();
    void *session;
    void *job;
    EnqueueHipFn fn;
    GetLastErrorFn getLastError = nullptr;
    bool match = true;
    {
        std::lock_guard lock(p.mutex);
        if (!p.targetList || p.targetList != list)
            return;
        if (!(p.enqueueHip && p.session && p.job))
        {
            p.skippedHits.fetch_add(1, std::memory_order_relaxed);
            p.lastEnqueueError = {};
            p.lastEnqueueQueue = nullptr;
            p.lastEnqueueRc.store(kEnqueueSkipped, std::memory_order_relaxed);
            p.targetList = nullptr;
            p.expectedQueue = nullptr;
            return;
        }
        if (p.expectedQueue && queue)
        {
            match = (queue == p.expectedQueue);
            if (!match)
            {
                IUnknown *id1 = nullptr;
                IUnknown *id2 = nullptr;
                queue->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&id1));
                p.expectedQueue->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&id2));
                match = (id1 && id2 && id1 == id2);
                if (id1) id1->Release();
                if (id2) id2->Release();
            }
            if (!match)
            {
                p.skippedHits.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Consume before call so a nested submission cannot enqueue twice.
        session = p.session;
        job = p.job;
        fn = p.enqueueHip;
        getLastError = p.getLastError;
        p.session = nullptr;
        p.job = nullptr;
        p.enqueueHip = nullptr;
        p.getLastError = nullptr;
        p.targetList = nullptr;
        p.expectedQueue = nullptr;
    }
    p.betweenHits.fetch_add(1, std::memory_order_relaxed);
    p.enqueueCalls.fetch_add(1, std::memory_order_relaxed);
    const int32_t rc = fn ? fn(session, job, queue) : -1;
    std::array<char, 256> error {};
    if (getLastError)
        getLastError(error.data(), static_cast<uint32_t>(error.size()));
    error.back() = 0;
    {
        std::lock_guard lock(p.mutex);
        if (!match)
        {
            if (rc == 0)
            {
                p.lastEnqueueRc.store(kEnqueueQueueMismatch, std::memory_order_relaxed);
                if (error[0] != 0)
                    p.lastEnqueueError = error;
                else
                {
                    std::array<char, 256> errBuf {};
                    std::snprintf(errBuf.data(), errBuf.size(),
                                  "command queue %p does not match session queue (output zeroed for original Color passthrough)",
                                  reinterpret_cast<void *>(queue));
                    p.lastEnqueueError = errBuf;
                }
            }
            else
            {
                p.lastEnqueueRc.store(rc, std::memory_order_relaxed);
                p.lastEnqueueError = error;
            }
        }
        else
        {
            p.lastEnqueueError = error;
            p.lastEnqueueRc.store(rc, std::memory_order_relaxed);
        }
        p.lastEnqueueQueue = queue;
    }
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

inline void SetPendingEnqueue(void *session, void *job, EnqueueHipFn enqueueHip, GetLastErrorFn getLastError,
                             ID3D12CommandList *targetList, ID3D12CommandQueue *expectedQueue = nullptr)
{
    auto &p = Pending();
    std::lock_guard lock(p.mutex);
    p.session = session;
    p.job = job;
    p.enqueueHip = enqueueHip;
    p.getLastError = getLastError;
    p.targetList = targetList;
    p.expectedQueue = expectedQueue;
}

struct EnqueueDiagnostic
{
    int32_t rc = 0;
    std::array<char, 256> error {};
    ID3D12CommandQueue *queue = nullptr;
};

inline EnqueueDiagnostic LastEnqueueDiagnostic()
{
    auto &p = Pending();
    std::lock_guard lock(p.mutex);
    return { p.lastEnqueueRc.load(std::memory_order_relaxed), p.lastEnqueueError, p.lastEnqueueQueue };
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
