#pragma once
#include "SubmissionTls.h"
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
        if (waitAll && retireFenceValue > done)
        {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (ev)
            {
                if (SUCCEEDED(retireFence->SetEventOnCompletion(retireFenceValue, ev)))
                {
                    const DWORD wr = WaitForSingleObject(ev, 30000);
                    (void)wr; // success or not: always re-read completed value below
                }
                CloseHandle(ev);
            }
            // Never treat the target fence value as done unless GetCompletedValue says so.
            done = retireFence->GetCompletedValue();
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
        return retireFence->GetCompletedValue() < retireFenceValue;
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

    HRESULT Split()
    {
        if (phase != Phase::RecordingProducer || !device || !producer)
            return E_UNEXPECTED;
        const HRESULT close = producer->Close();
        if (FAILED(close))
            return close;
        HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&contAlloc));
        if (FAILED(hr))
            return hr;
        // Continuation must be a real list, not another proxy (hook re-entrancy).
        SuppressProxyWrap suppress;
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, contAlloc, nullptr,
                                       IID_PPV_ARGS(&continuation));
        if (FAILED(hr))
            return hr;
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

    // between is invoked after producer Execute and before continuation Execute when split.
    // Used as the HIP insert slot. nullptr = no work between the two Executes.
    // Closed lists may be re-submitted after prior GPU work (D3D12 allows multiple Execute).
    HRESULT Execute(ID3D12CommandQueue *queue, void (*between)(void *) = nullptr, void *betweenCtx = nullptr)
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

        // Bypass Detoured ECL (AmdBridge Submitted / expand). Internal producer submit
        // must not ClearPendingEnqueue before the HIP between-slot runs.
        LogicalExecuteScope logicalExecScope;
        ID3D12CommandList *first = producer;
        if (g_rawExecuteCommandLists)
            g_rawExecuteCommandLists(queue, 1, &first);
        else
            queue->ExecuteCommandLists(1, &first);
        if (split && between)
            between(betweenCtx);
        if (split && continuation)
        {
            ID3D12CommandList *second = continuation;
            if (g_rawExecuteCommandLists)
                g_rawExecuteCommandLists(queue, 1, &second);
            else
                queue->ExecuteCommandLists(1, &second);
        }
        // Completion credential for deferred continuation allocator recycle.
        const UINT64 v = ++retireFenceValue;
        hr = queue->Signal(retireFence, v);
        executed = true;
        // Signal failure: GPU work is already submitted; keep resources (fence never reaches v).
        if (FAILED(hr))
            return hr;
        return S_OK;
    }

    HRESULT Reset(ID3D12CommandAllocator *alloc, ID3D12PipelineState *initial)
    {
        if (!producer || !alloc)
            return E_INVALIDARG;
        // Still recording: cannot Reset (matches "must be closed" spirit).
        if (phase == Phase::RecordingProducer || phase == Phase::RecordingContinuation)
            return E_UNEXPECTED;
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
        if (producerAlloc)
            producerAlloc->Release();
        producerAlloc = alloc;
        producerAlloc->AddRef();
        const HRESULT hr = producer->Reset(alloc, initial);
        if (FAILED(hr))
            return hr;
        phase = Phase::RecordingProducer;
        split = false;
        executed = false;
        ++generation;
        return S_OK;
    }

    void Release()
    {
        FlushRetired(true);
        // If wait failed or GPU still busy, park current continuation then abandon unfinished refs.
        if ((continuation || contAlloc) && ContinuationStillInFlight())
            RetireCurrentContinuation(retireFenceValue);
        const bool abandon = !retired.empty() || ContinuationStillInFlight();
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
            phase = Phase::Idle;
            split = false;
            executed = false;
            retireFenceValue = 0;
            return;
        }
        IUnknown *cont = continuation;
        IUnknown *ca = contAlloc;
        IUnknown *prod = producer;
        IUnknown *pa = producerAlloc;
        IUnknown *dev = device;
        IUnknown *ff = retireFence;
        continuation = nullptr;
        contAlloc = nullptr;
        producer = nullptr;
        producerAlloc = nullptr;
        device = nullptr;
        retireFence = nullptr;
        ReleaseIf(cont);
        ReleaseIf(ca);
        ReleaseIf(prod);
        ReleaseIf(pa);
        ReleaseIf(ff);
        ReleaseIf(dev);
        phase = Phase::Idle;
        split = false;
        executed = false;
        retireFenceValue = 0;
    }
};
} // namespace DlssNr::Submission
