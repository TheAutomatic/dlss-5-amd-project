#pragma once
#include "../amd/GraphicsTracker.h"
#include "SubmissionTls.h"
#include "RecordingLifecycle.h"
#include <d3d12.h>
#include <cstdint>
#include <vector>

namespace DlssNr::Submission
{
enum class Phase : uint32_t
{
    Idle = 0,
    RecordingProducer = 1,
    RecordingContinuation = 2,
    Closed = 3,
};

// Optional between-slot callback executed after the producer command list has been submitted
// to the queue, but before the continuation command list is submitted.
using BetweenCallback = void (*)(ID3D12CommandQueue *queue, void *ctx);

// Bookkeeping for one logical list mapped to producer + optional continuation.
// Not a COM proxy: callers record on Current(). Game wrapping is a later increment.
class LogicalList
{
    struct RetiredCont
    {
        ID3D12CommandAllocator *alloc = nullptr;
        ID3D12GraphicsCommandList *list = nullptr;
        UINT64 fenceValue = 0;
    };

    ID3D12Device *device = nullptr;
    ID3D12CommandAllocator *producerAlloc = nullptr;
    ID3D12GraphicsCommandList *producer = nullptr;
    ID3D12CommandAllocator *contAlloc = nullptr;
    ID3D12GraphicsCommandList *continuation = nullptr;
    ID3D12Fence *retireFence = nullptr;
    UINT64 retireFenceValue = 0;
    std::vector<RetiredCont> retired;
    uint64_t generation = 0;
    Phase phase = Phase::Idle;
    bool split = false;
    bool executed = false;
    bool unconfirmedSubmission = false;
    ID3D12CommandQueue* lastQueue = nullptr;

    static void ReleaseIf(IUnknown *&p)
    {
        if (p)
        {
            p->Release();
            p = nullptr;
        }
    }

    HRESULT EnsureRetireFence()
    {
        if (retireFence || !device)
            return retireFence ? S_OK : E_UNEXPECTED;
        return device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retireFence));
    }

    // Free only entries whose fence value is actually complete.
    // On wait failure/timeout, keep ownership of everything still in flight.
    void FlushRetired(bool waitAll)
    {
        if (!retireFence)
            return;
        UINT64 done = retireFence->GetCompletedValue();
        if (done == UINT64_MAX || unconfirmedSubmission)
            return; // Device removal/failed Signal is not an ordinary completion.
        if (waitAll && retireFenceValue > done)
        {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (ev)
            {
                const HRESULT armed = retireFence->SetEventOnCompletion(retireFenceValue, ev);
                if (FAILED(armed) || WaitForSingleObject(ev, 30000) == WAIT_OBJECT_0)
                    CloseHandle(ev);
                // A timeout/failed wait does not cancel SetEventOnCompletion.
                // Retain an armed handle rather than letting a later GPU signal
                // target a closed (possibly recycled) Windows handle.
            }
            // Never treat the target fence value as done unless GetCompletedValue says so.
            done = retireFence->GetCompletedValue();
            if (done == UINT64_MAX)
                return;
        }
        size_t w = 0;
        for (size_t i = 0; i < retired.size(); ++i)
        {
            if (retired[i].fenceValue <= done)
            {
                IUnknown *a = retired[i].alloc;
                IUnknown *l = retired[i].list;
                retired[i].alloc = nullptr;
                retired[i].list = nullptr;
                ReleaseIf(a);
                ReleaseIf(l);
            }
            else
            {
                if (w != i)
                    retired[w] = retired[i];
                ++w;
            }
        }
        retired.resize(w);
    }

    // Intentionally leak COM refs so GPU-live allocators are never freed on a failed wait.
    void AbandonUnfinishedRetired()
    {
        for (size_t i = 0; i < retired.size(); ++i)
        {
            retired[i].alloc = nullptr;
            retired[i].list = nullptr;
        }
        retired.clear();
    }

    bool ContinuationStillInFlight() const
    {
        if (!retireFence || retireFenceValue == 0)
            return false;
        const auto done = retireFence->GetCompletedValue();
        return unconfirmedSubmission || done == UINT64_MAX || done < retireFenceValue;
    }

    void RetireCurrentContinuation(UINT64 fenceValue)
    {
        if (!contAlloc && !continuation)
            return;
        RetiredCont r {};
        r.alloc = contAlloc;
        r.list = continuation;
        r.fenceValue = fenceValue;
        contAlloc = nullptr;
        continuation = nullptr;
        retired.push_back(r);
    }

  public:
    ~LogicalList() { Release(); }

    LogicalList() = default;
    LogicalList(const LogicalList &) = delete;
    LogicalList &operator=(const LogicalList &) = delete;

    uint64_t Generation() const { return generation; }
    Phase GetPhase() const { return phase; }
    bool WasSplit() const { return split; }

    ID3D12GraphicsCommandList *Current() const
    {
        if (phase == Phase::RecordingContinuation)
            return continuation;
        // Closed (incl. CreateCommandList1 before first Reset) still exposes producer for GetDevice/QI.
        if (phase == Phase::RecordingProducer || phase == Phase::Closed)
            return producer;
        return nullptr;
    }

    HRESULT BindProducer(ID3D12Device *dev, ID3D12CommandAllocator *alloc, ID3D12GraphicsCommandList *list)
    {
        if (!dev || !alloc || !list)
            return E_INVALIDARG;
        Release();
        device = dev;
        device->AddRef();
        producerAlloc = alloc;
        producerAlloc->AddRef();
        producer = list;
        producer->AddRef();
        phase = Phase::RecordingProducer;
        split = false;
        executed = false;
        ++generation;
        return S_OK;
    }

    // CreateCommandList1: closed list, allocator arrives on first Reset.
    HRESULT BindClosedProducer(ID3D12Device *dev, ID3D12GraphicsCommandList *list)
    {
        if (!dev || !list)
            return E_INVALIDARG;
        Release();
        device = dev;
        device->AddRef();
        producer = list;
        producer->AddRef();
        producerAlloc = nullptr;
        phase = Phase::Closed;
        split = false;
        executed = false;
        ++generation;
        return S_OK;
    }

    HRESULT Split(bool needsList4 = false)
    {
        if (phase != Phase::RecordingProducer || !device || !producer)
            return E_UNEXPECTED;
        // Allocate before closing the producer: allocation failure must leave recording usable.
        ID3D12CommandAllocator *nextAlloc = nullptr;
        ID3D12GraphicsCommandList *nextList = nullptr;
        HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&nextAlloc));
        if (FAILED(hr))
            return hr;
        // Continuation must be a real list, not another proxy (hook re-entrancy).
        SuppressProxyWrap suppress;
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, nextAlloc, nullptr,
                                       IID_PPV_ARGS(&nextList));
        if (FAILED(hr))
        {
            nextAlloc->Release();
            return hr;
        }
        // Check before closing the producer: failure must preserve a usable list.
        if (needsList4)
        {
            ID3D12GraphicsCommandList4 *l4 = nullptr;
            hr = nextList->QueryInterface(IID_PPV_ARGS(&l4));
            if (FAILED(hr) || !l4)
            {
                nextList->Release(); nextAlloc->Release();
                return FAILED(hr) ? hr : E_NOINTERFACE;
            }
            l4->Release();
        }
        hr = producer->Close();
        if (FAILED(hr))
        {
            nextList->Release();
            nextAlloc->Release();
            return hr;
        }
        AmdPreSr::GraphicsSnap::GraphicsTracker().OnCreate(reinterpret_cast<uint64_t>(nextList));
        contAlloc = nextAlloc;
        continuation = nextList;
        phase = Phase::RecordingContinuation;
        split = true;
        return S_OK;
    }

    HRESULT Close()
    {
        ID3D12GraphicsCommandList *cur = Current();
        if (!cur)
            return phase == Phase::Closed ? S_OK : E_UNEXPECTED;
        const HRESULT hr = cur->Close();
        if (FAILED(hr))
            return hr;
        phase = Phase::Closed;
        return S_OK;
    }

    // Optional between-slot callback executed after the producer command list has been submitted
    // to the queue, but before the continuation command list is submitted.
    using BetweenCallback = void (*)(ID3D12CommandQueue *queue, void *ctx);

    // Closed lists may be re-submitted after prior GPU work (D3D12 allows multiple Execute).
    HRESULT Execute(ID3D12CommandQueue *queue, BetweenCallback between = nullptr, void *betweenCtx = nullptr,
                    RecordingExecution* facts = nullptr, RecordingObserver* observer = nullptr)
    {
        if (!queue || !producer)
            return E_INVALIDARG;
        if (phase == Phase::RecordingProducer || phase == Phase::RecordingContinuation)
        {
            const HRESULT hr = Close();
            if (FAILED(hr))
                return hr;
        }
        if (phase != Phase::Closed)
            return E_UNEXPECTED;
        // Fence must exist before submit so Reset/Release can retain on failure.
        HRESULT hr = EnsureRetireFence();
        if (FAILED(hr))
            return hr;
        // Never let a later signal from another queue falsely complete an older
        // submission. Cross-queue executions of the same list are GPU-ordered.
        if (unconfirmedSubmission)
            return E_FAIL;
        if (lastQueue && lastQueue != queue && retireFenceValue)
        {
            hr = queue->Wait(retireFence, retireFenceValue);
            if (FAILED(hr)) return hr;
        }
        if (observer && facts)
        {
            hr = observer->BeforeExecute(*facts);
            if (FAILED(hr)) return hr;
        }

        // Bypass Detoured ECL (AmdBridge Submitted / expand). Internal producer submit
        // must not ClearPendingEnqueue before the HIP between-slot runs.
        LogicalExecuteScope logicalExecScope;
        ID3D12CommandList *first = producer;
        if (g_rawExecuteCommandLists)
            g_rawExecuteCommandLists(queue, 1, &first);
        else
            queue->ExecuteCommandLists(1, &first);
        if (facts)
        {
            facts->producerSubmitted = true;
            if (observer) observer->ProducerSubmitted(*facts);
        }
        if (split)
            g_splitSubmissions.fetch_add(1, std::memory_order_relaxed);
        if (split && between)
            between(queue, betweenCtx);
        if (split && observer && facts)
            observer->Between(*facts);
        if (split && continuation)
        {
            ID3D12CommandList *second = continuation;
            if (g_rawExecuteCommandLists)
                g_rawExecuteCommandLists(queue, 1, &second);
            else
                queue->ExecuteCommandLists(1, &second);
            g_continuationSubmissions.fetch_add(1, std::memory_order_relaxed);
            if (facts) facts->continuationSubmitted = true;
        }
        // Completion credential for deferred continuation allocator recycle.
        const UINT64 v = ++retireFenceValue;
        hr = queue->Signal(retireFence, v);
        executed = true;
        if (lastQueue != queue)
        {
            queue->AddRef();
            if (lastQueue) lastQueue->Release();
            lastQueue = queue;
        }
        if (facts)
        {
            facts->fence = retireFence;
            facts->fenceValue = v;
        }
        // Signal failure: GPU work is already submitted; keep resources (fence never reaches v).
        if (FAILED(hr))
        {
            unconfirmedSubmission = true;
            return hr;
        }
        return S_OK;
    }

    HRESULT Reset(ID3D12CommandAllocator *alloc, ID3D12PipelineState *initial)
    {
        if (!producer || !alloc)
            return E_INVALIDARG;
        // Still recording: cannot Reset (matches "must be closed" spirit).
        if (phase == Phase::RecordingProducer || phase == Phase::RecordingContinuation)
            return E_UNEXPECTED;
        // A rejected Reset must preserve the closed generation (including its continuation).
        const HRESULT hr = producer->Reset(alloc, initial);
        if (FAILED(hr))
            return hr;
        // CreateCommandList1 path: first Reset supplies the producer allocator.
        // Closed never-executed (Create→Close→Reset) and post-Execute are both OK.
        FlushRetired(false);
        if (continuation || contAlloc)
        {
            // Do not free GPU-live continuation storage; retire until fence completes.
            // hold==0 means never submitted under a retire fence (e.g. Close without Execute) — safe to free.
            if (ContinuationStillInFlight())
                RetireCurrentContinuation(retireFenceValue);
            else
            {
                IUnknown *c = continuation;
                IUnknown *a = contAlloc;
                continuation = nullptr;
                contAlloc = nullptr;
                ReleaseIf(c);
                ReleaseIf(a);
            }
        }
        alloc->AddRef();
        if (producerAlloc)
            producerAlloc->Release();
        producerAlloc = alloc;
        phase = Phase::RecordingProducer;
        split = false;
        executed = false;
        ++generation;
        return S_OK;
    }

    void Release()
    {
        const bool deviceLost = device && FAILED(device->GetDeviceRemovedReason());
        if (deviceLost)
        {
            // Removed-device teardown is separate from normal fence completion.
            for (auto& r : retired) { if (r.list) r.list->Release(); if (r.alloc) r.alloc->Release(); }
            retired.clear();
        }
        else FlushRetired(true);
        // If wait failed or GPU still busy, park current continuation then abandon unfinished refs.
        if (!deviceLost && (continuation || contAlloc) && ContinuationStillInFlight())
            RetireCurrentContinuation(retireFenceValue);
        const bool abandon = !deviceLost && (!retired.empty() || ContinuationStillInFlight());
        if (abandon)
        {
            AbandonUnfinishedRetired();
            // Drop pointers without Release — fail-closed intentional COM leak.
            continuation = nullptr;
            contAlloc = nullptr;
            producer = nullptr;
            producerAlloc = nullptr;
            device = nullptr;
            retireFence = nullptr;
            lastQueue = nullptr;
            phase = Phase::Idle;
            split = false;
            executed = false;
            retireFenceValue = 0;
            unconfirmedSubmission = false;
            return;
        }
        IUnknown *cont = continuation;
        IUnknown *ca = contAlloc;
        IUnknown *prod = producer;
        IUnknown *pa = producerAlloc;
        IUnknown *dev = device;
        IUnknown *ff = retireFence;
        IUnknown *q = lastQueue;
        continuation = nullptr;
        contAlloc = nullptr;
        producer = nullptr;
        producerAlloc = nullptr;
        device = nullptr;
        retireFence = nullptr;
        lastQueue = nullptr;
        ReleaseIf(cont);
        ReleaseIf(ca);
        ReleaseIf(prod);
        ReleaseIf(pa);
        ReleaseIf(ff);
        ReleaseIf(dev);
        ReleaseIf(q);
        phase = Phase::Idle;
        split = false;
        executed = false;
        retireFenceValue = 0;
        unconfirmedSubmission = false;
    }
};
} // namespace DlssNr::Submission
