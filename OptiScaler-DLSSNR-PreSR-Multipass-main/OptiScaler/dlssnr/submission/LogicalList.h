#pragma once
#include <d3d12.h>
#include <cstdint>

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
    ID3D12Device *device = nullptr;
    ID3D12CommandAllocator *producerAlloc = nullptr;
    ID3D12GraphicsCommandList *producer = nullptr;
    ID3D12CommandAllocator *contAlloc = nullptr;
    ID3D12GraphicsCommandList *continuation = nullptr;
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
        if (phase == Phase::RecordingProducer)
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

    HRESULT Execute(ID3D12CommandQueue *queue)
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
        if (executed)
            return E_UNEXPECTED;
        ID3D12CommandList *first = producer;
        queue->ExecuteCommandLists(1, &first);
        if (split && continuation)
        {
            ID3D12CommandList *second = continuation;
            queue->ExecuteCommandLists(1, &second);
        }
        executed = true;
        return S_OK;
    }

    HRESULT Reset(ID3D12CommandAllocator *alloc, ID3D12PipelineState *initial)
    {
        if (!producer || !alloc)
            return E_INVALIDARG;
        if (!executed && phase != Phase::Idle)
            return E_UNEXPECTED;
        if (continuation)
        {
            continuation->Release();
            continuation = nullptr;
        }
        if (contAlloc)
        {
            contAlloc->Release();
            contAlloc = nullptr;
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
        IUnknown *cont = continuation;
        IUnknown *ca = contAlloc;
        IUnknown *prod = producer;
        IUnknown *pa = producerAlloc;
        IUnknown *dev = device;
        continuation = nullptr;
        contAlloc = nullptr;
        producer = nullptr;
        producerAlloc = nullptr;
        device = nullptr;
        ReleaseIf(cont);
        ReleaseIf(ca);
        ReleaseIf(prod);
        ReleaseIf(pa);
        ReleaseIf(dev);
        phase = Phase::Idle;
        split = false;
        executed = false;
    }
};
} // namespace DlssNr::Submission
