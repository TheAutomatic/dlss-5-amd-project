#pragma once
#include "LogicalList.h"
#include "ContinuationState.h"
#include "ResourceStateBook.h"
#include <atomic>

// COM proxy for ID3D12GraphicsCommandList1..10 (inherits List10).
// QI accepts List1..List10 + base. Newer methods QI the live producer; if unsupported, fail-closed (no-op / E_UNEXPECTED).
// Create/Execute wrap helpers live in SubmissionHooks.h (armed only by harness / future P3).
namespace DlssNr::Submission
{
MIDL_INTERFACE("b3c0e9a1-4d2f-4c77-9a18-6f2d8e1b4c01")
ILogicalCommandList : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SplitSegments(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExecuteOn(ID3D12CommandQueue *queue) = 0;
    // HIP / NR slot between producer and continuation Executes. Pass nullptr for no-op.
    virtual HRESULT STDMETHODCALLTYPE ExecuteOnWithBetween(ID3D12CommandQueue *queue, void (*between)(void *),
                                                           void *betweenCtx) = 0;
    virtual bool STDMETHODCALLTYPE IsSplitIneligible(void) = 0;
    // Harness: viewport count captured for continuation seed (0 if never set).
    virtual UINT STDMETHODCALLTYPE CapturedViewportCount(void) = 0;
    // Harness: plan D continuation IA capture.
    virtual BOOL STDMETHODCALLTYPE CapturedIbBound(void) = 0;
    virtual UINT STDMETHODCALLTYPE CapturedVbSlotCount(void) = 0;
    virtual UINT STDMETHODCALLTYPE CapturedGfxRootCount(void) = 0;
    virtual BOOL STDMETHODCALLTYPE CapturedSamplePositions(void) = 0;
    // Borrowed pointer, only for unsplit lists: preserve the caller's Execute batch.
    virtual ID3D12CommandList *STDMETHODCALLTYPE UnsplitNativeList(void) = 0;
    virtual const char *STDMETHODCALLTYPE SplitRejectionReason(void) = 0;
};

class CommandListProxy final : public ID3D12GraphicsCommandList10, public ILogicalCommandList
{
    std::atomic<ULONG> refs { 1 };
    LogicalList logical;
    ContinuationState contState;
    ResourceStateBook resBook;
    bool splitIneligible = false;
    std::atomic<bool> rawInterfaceEscaped { false }; // Lifetime-wide; an alias can outlive Reset.
    const char *splitIneligibleReason = nullptr;

    void MarkSplitIneligible(const char *reason)
    {
        splitIneligible = true;
        if (!splitIneligibleReason)
            splitIneligibleReason = reason;
    }


    ID3D12GraphicsCommandList *Cur() const { return logical.Current(); }

    template <typename TIface>
    TIface *CurAs() const
    {
        ID3D12GraphicsCommandList *c = Cur();
        if (!c)
            return nullptr;
        TIface *p = nullptr;
        if (FAILED(c->QueryInterface(__uuidof(TIface), reinterpret_cast<void **>(&p))))
            return nullptr;
        return p; // caller must Release
    }

  public:
    static HRESULT Create(ID3D12Device *device, ID3D12CommandAllocator *alloc, ID3D12GraphicsCommandList *real,
                          CommandListProxy **out, ID3D12PipelineState *initial = nullptr)
    {
        if (!device || !alloc || !real || !out)
            return E_INVALIDARG;
        auto *p = new CommandListProxy();
        const HRESULT hr = p->logical.BindProducer(device, alloc, real);
        if (FAILED(hr))
        {
            delete p;
            return hr;
        }
        if (initial)
            p->contState.OnPso(initial);
        *out = p;
        return S_OK;
    }

    // CreateCommandList1: real list is closed; allocator bound on first Reset.
    static HRESULT CreateClosed(ID3D12Device *device, ID3D12GraphicsCommandList *real, CommandListProxy **out)
    {
        if (!device || !real || !out)
            return E_INVALIDARG;
        auto *p = new CommandListProxy();
        const HRESULT hr = p->logical.BindClosedProducer(device, real);
        if (FAILED(hr))
        {
            delete p;
            return hr;
        }
        *out = p;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;
        // COM identity: IUnknown / list IIDs / our logical face stay on the proxy.
        if (riid == IID_IUnknown || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12CommandList) || riid == __uuidof(ID3D12GraphicsCommandList) ||
            riid == __uuidof(ID3D12GraphicsCommandList1) || riid == __uuidof(ID3D12GraphicsCommandList2) ||
            riid == __uuidof(ID3D12GraphicsCommandList3) || riid == __uuidof(ID3D12GraphicsCommandList4) ||
            riid == __uuidof(ID3D12GraphicsCommandList5) || riid == __uuidof(ID3D12GraphicsCommandList6) ||
            riid == __uuidof(ID3D12GraphicsCommandList7) || riid == __uuidof(ID3D12GraphicsCommandList8) ||
            riid == __uuidof(ID3D12GraphicsCommandList9) || riid == __uuidof(ID3D12GraphicsCommandList10))
        {
            // Do not advertise interface versions unsupported by the native list.
            IUnknown *supported = nullptr;
            if (!Cur() || FAILED(Cur()->QueryInterface(riid, reinterpret_cast<void **>(&supported))))
                return E_NOINTERFACE;
            supported->Release();
            *ppv = static_cast<ID3D12GraphicsCommandList10 *>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(ILogicalCommandList))
        {
            *ppv = static_cast<ILogicalCommandList *>(this);
            AddRef();
            return S_OK;
        }
        // Streamline / debug / vendor QIs: forward to the live producer/continuation list
        // instead of E_NOINTERFACE (燕云 crash with ArmCreate + wrap).
        if (auto *cur = logical.Current())
        {
            // Unknown interfaces cannot be redirected to a continuation. Before a cut,
            // keep compatibility but permanently refuse splitting this object.
            if (logical.WasSplit())
                return E_NOINTERFACE;
            const HRESULT hr = cur->QueryInterface(riid, ppv);
            if (SUCCEEDED(hr))
                rawInterfaceEscaped = true;
            return hr;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (!n)
            delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *size, void *data) override
    {
        return logical.Current() ? logical.Current()->GetPrivateData(guid, size, data) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void *data) override
    {
        return logical.Current() ? logical.Current()->SetPrivateData(guid, size, data) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *data) override
    {
        return logical.Current() ? logical.Current()->SetPrivateDataInterface(guid, data) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override
    {
        return logical.Current() ? logical.Current()->SetName(name) : E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override
    {
        return logical.Current() ? logical.Current()->GetDevice(riid, device) : E_UNEXPECTED;
    }
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return D3D12_COMMAND_LIST_TYPE_DIRECT; }

    HRESULT STDMETHODCALLTYPE Close() override { return logical.Close(); }
    HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *alloc, ID3D12PipelineState *initial) override
    {
        const HRESULT hr = logical.Reset(alloc, initial);
        if (FAILED(hr))
            return hr;
        contState.Reset();
        resBook.Reset();
        splitIneligible = false;
        splitIneligibleReason = nullptr;
        if (initial)
            contState.OnPso(initial);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SplitSegments() override
    {
        if (IsSplitIneligible())
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        const char *why = nullptr;
        if (!resBook.CanSplit(&why))
        {
            MarkSplitIneligible(why ? why : "resource_state");
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        }
        const HRESULT hr = logical.Split();
        if (FAILED(hr))
            return hr;
        // Seed continuation with captured producer bindings (IA/SO/VRS included).
        contState.ApplyTo(logical.Current());
        // Producer Execute introduces a promotion/decay boundary: keep tracked
        // resources but apply Microsoft decay so continuation starts from the
        // post-Execute expectation (plan D / M3), not a wipe.
        resBook.ApplyExecuteDecay();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ExecuteOn(ID3D12CommandQueue *queue) override { return logical.Execute(queue); }
    HRESULT STDMETHODCALLTYPE ExecuteOnWithBetween(ID3D12CommandQueue *queue, void (*between)(void *),
                                                   void *betweenCtx) override
    {
        return logical.Execute(queue, between, betweenCtx);
    }
    bool STDMETHODCALLTYPE IsSplitIneligible() override
    {
        return rawInterfaceEscaped || splitIneligible || logical.WasSplit() || !resBook.CanSplit(nullptr);
    }
    ID3D12CommandList *STDMETHODCALLTYPE UnsplitNativeList() override
    {
        return logical.WasSplit() ? nullptr : logical.Current();
    }
    const char *STDMETHODCALLTYPE SplitRejectionReason() override
    {
        if (rawInterfaceEscaped) return "native_interface_escaped";
        if (logical.WasSplit()) return "already_split";
        if (splitIneligibleReason) return splitIneligibleReason;
        const char *reason = nullptr;
        resBook.CanSplit(&reason);
        return reason ? reason : "eligible";
    }
    const char *SplitIneligibleReason() const { return splitIneligibleReason; }
    UINT STDMETHODCALLTYPE CapturedViewportCount() override
    {
        return contState.hasViewports ? contState.numViewports : 0;
    }
    BOOL STDMETHODCALLTYPE CapturedIbBound() override { return contState.hasIb ? TRUE : FALSE; }
    UINT STDMETHODCALLTYPE CapturedVbSlotCount() override { return contState.CapturedVbSlotCount(); }
    UINT STDMETHODCALLTYPE CapturedGfxRootCount() override { return contState.gfxRoots.BoundCount(); }
    BOOL STDMETHODCALLTYPE CapturedSamplePositions() override
    {
        return contState.hasSamplePositions ? TRUE : FALSE;
    }

    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState *p) override
    {
        contState.Reset();
        if (p)
            contState.OnPso(p);
        if (auto *c = Cur())
            c->ClearState(p);
    }
    void STDMETHODCALLTYPE DrawInstanced(UINT a, UINT b, UINT d, UINT e) override
    {
        if (auto *c = Cur())
            c->DrawInstanced(a, b, d, e);
    }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT a, UINT b, UINT d, INT e, UINT f) override
    {
        if (auto *c = Cur())
            c->DrawIndexedInstanced(a, b, d, e, f);
    }
    void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override
    {
        if (auto *c = Cur())
            c->Dispatch(x, y, z);
    }
    void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource *d, UINT64 o, ID3D12Resource *s, UINT64 so, UINT64 n) override
    {
        if (auto *c = Cur())
            c->CopyBufferRegion(d, o, s, so, n);
    }
    void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *d, UINT x, UINT y, UINT z,
                                             const D3D12_TEXTURE_COPY_LOCATION *s, const D3D12_BOX *b) override
    {
        if (auto *c = Cur())
            c->CopyTextureRegion(d, x, y, z, s, b);
    }
    void STDMETHODCALLTYPE CopyResource(ID3D12Resource *d, ID3D12Resource *s) override
    {
        if (auto *c = Cur())
            c->CopyResource(d, s);
    }
    void STDMETHODCALLTYPE CopyTiles(ID3D12Resource *t, const D3D12_TILED_RESOURCE_COORDINATE *c0,
                                     const D3D12_TILE_REGION_SIZE *sz, ID3D12Resource *buf, UINT64 off,
                                     D3D12_TILE_COPY_FLAGS flags) override
    {
        if (auto *c = Cur())
            c->CopyTiles(t, c0, sz, buf, off, flags);
    }
    void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource *d, UINT ds, ID3D12Resource *s, UINT ss,
                                              DXGI_FORMAT f) override
    {
        if (auto *c = Cur())
            c->ResolveSubresource(d, ds, s, ss, f);
    }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY t) override
    {
        contState.OnTopology(t);
        if (auto *c = Cur())
            c->IASetPrimitiveTopology(t);
    }
    void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D12_VIEWPORT *v) override
    {
        contState.OnViewports(n, v);
        if (auto *c = Cur())
            c->RSSetViewports(n, v);
    }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D12_RECT *r) override
    {
        contState.OnScissors(n, r);
        if (auto *c = Cur())
            c->RSSetScissorRects(n, r);
    }
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT f[4]) override
    {
        contState.OnBlend(f);
        if (auto *c = Cur())
            c->OMSetBlendFactor(f);
    }
    void STDMETHODCALLTYPE OMSetStencilRef(UINT s) override
    {
        contState.OnStencil(s);
        if (auto *c = Cur())
            c->OMSetStencilRef(s);
    }
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState *p) override
    {
        contState.OnPso(p);
        if (auto *c = Cur())
            c->SetPipelineState(p);
    }
    void STDMETHODCALLTYPE ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER *b) override
    {
        const char *why = nullptr;
        if (!resBook.OnBarriers(n, b, &why))
            MarkSplitIneligible(why ? why : "barrier");
        if (auto *c = Cur())
            c->ResourceBarrier(n, b);
    }
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *l) override
    {
        MarkSplitIneligible("bundle_state_not_captured");
        if (auto *c = Cur())
            c->ExecuteBundle(l);
    }
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap *const *h) override
    {
        contState.OnHeaps(n, h);
        if (auto *c = Cur())
            c->SetDescriptorHeaps(n, h);
    }
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature *s) override
    {
        contState.OnComputeRoot(s);
        if (auto *c = Cur())
            c->SetComputeRootSignature(s);
    }
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *s) override
    {
        contState.OnGfxRoot(s);
        if (auto *c = Cur())
            c->SetGraphicsRootSignature(s);
    }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) override
    {
        if (!contState.computeRoots.OnTable(i, h))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetComputeRootDescriptorTable(i, h);
    }
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) override
    {
        if (!contState.gfxRoots.OnTable(i, h))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetGraphicsRootDescriptorTable(i, h);
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT i, UINT v, UINT o) override
    {
        if (!contState.computeRoots.OnSingleConstant(i, v, o))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetComputeRoot32BitConstant(i, v, o);
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT i, UINT v, UINT o) override
    {
        if (!contState.gfxRoots.OnSingleConstant(i, v, o))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetGraphicsRoot32BitConstant(i, v, o);
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT i, UINT n, const void *s, UINT o) override
    {
        if (!contState.computeRoots.OnConstants(i, n, s, o))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetComputeRoot32BitConstants(i, n, s, o);
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT i, UINT n, const void *s, UINT o) override
    {
        if (!contState.gfxRoots.OnConstants(i, n, s, o))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetGraphicsRoot32BitConstants(i, n, s, o);
    }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (!contState.computeRoots.OnGpuVa(i, RootBindState::EntryType::CBV, a))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetComputeRootConstantBufferView(i, a);
    }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (!contState.gfxRoots.OnGpuVa(i, RootBindState::EntryType::CBV, a))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetGraphicsRootConstantBufferView(i, a);
    }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (!contState.computeRoots.OnGpuVa(i, RootBindState::EntryType::SRV, a))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetComputeRootShaderResourceView(i, a);
    }
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (!contState.gfxRoots.OnGpuVa(i, RootBindState::EntryType::SRV, a))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetGraphicsRootShaderResourceView(i, a);
    }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (!contState.computeRoots.OnGpuVa(i, RootBindState::EntryType::UAV, a))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetComputeRootUnorderedAccessView(i, a);
    }
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (!contState.gfxRoots.OnGpuVa(i, RootBindState::EntryType::UAV, a))
            MarkSplitIneligible("root_overflow");
        if (auto *c = Cur())
            c->SetGraphicsRootUnorderedAccessView(i, a);
    }
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *v) override
    {
        contState.OnIndexBuffer(v);
        if (auto *c = Cur())
            c->IASetIndexBuffer(v);
    }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT s, UINT n, const D3D12_VERTEX_BUFFER_VIEW *v) override
    {
        contState.OnVertexBuffers(s, n, v);
        if (auto *c = Cur())
            c->IASetVertexBuffers(s, n, v);
    }
    void STDMETHODCALLTYPE SOSetTargets(UINT s, UINT n, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *v) override
    {
        contState.OnSoTargets(s, n, v);
        if (auto *c = Cur())
            c->SOSetTargets(s, n, v);
    }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *rt, BOOL single,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE *ds) override
    {
        contState.OnOm(n, rt, single, ds);
        if (auto *c = Cur())
            c->OMSetRenderTargets(n, rt, single, ds);
    }
    void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE h, D3D12_CLEAR_FLAGS f, FLOAT d, UINT8 s,
                                                 UINT n, const D3D12_RECT *r) override
    {
        if (auto *c = Cur())
            c->ClearDepthStencilView(h, f, d, s, n, r);
    }
    void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE h, const FLOAT c4[4], UINT n,
                                                 const D3D12_RECT *r) override
    {
        if (auto *c = Cur())
            c->ClearRenderTargetView(h, c4, n, r);
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                                        ID3D12Resource *res, const UINT v[4], UINT n,
                                                        const D3D12_RECT *r) override
    {
        if (auto *c = Cur())
            c->ClearUnorderedAccessViewUint(g, cpu, res, v, n, r);
    }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE g, D3D12_CPU_DESCRIPTOR_HANDLE cpu,
                                                         ID3D12Resource *res, const FLOAT v[4], UINT n,
                                                         const D3D12_RECT *r) override
    {
        if (auto *c = Cur())
            c->ClearUnorderedAccessViewFloat(g, cpu, res, v, n, r);
    }
    void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *r, const D3D12_DISCARD_REGION *region) override
    {
        if (auto *c = Cur())
            c->DiscardResource(r, region);
    }
    void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap *h, D3D12_QUERY_TYPE t, UINT i) override
    {
        MarkSplitIneligible("query");
        if (auto *c = Cur())
            c->BeginQuery(h, t, i);
    }
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *h, D3D12_QUERY_TYPE t, UINT i) override
    {
        MarkSplitIneligible("query");
        if (auto *c = Cur())
            c->EndQuery(h, t, i);
    }
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *h, D3D12_QUERY_TYPE t, UINT s, UINT n, ID3D12Resource *d,
                                            UINT64 o) override
    {
        MarkSplitIneligible("query");
        if (auto *c = Cur())
            c->ResolveQueryData(h, t, s, n, d, o);
    }
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource *b, UINT64 o, D3D12_PREDICATION_OP op) override
    {
        MarkSplitIneligible("predication");
        if (auto *c = Cur())
            c->SetPredication(b, o, op);
    }
    void STDMETHODCALLTYPE SetMarker(UINT m, const void *p, UINT s) override
    {
        if (auto *c = Cur())
            c->SetMarker(m, p, s);
    }
    void STDMETHODCALLTYPE BeginEvent(UINT m, const void *p, UINT s) override
    {
        if (auto *c = Cur())
            c->BeginEvent(m, p, s);
    }
    void STDMETHODCALLTYPE EndEvent() override
    {
        if (auto *c = Cur())
            c->EndEvent();
    }
    void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature *sig, UINT n, ID3D12Resource *arg, UINT64 ao,
                                           ID3D12Resource *cnt, UINT64 co) override
    {
        if (auto *c = Cur())
            c->ExecuteIndirect(sig, n, arg, ao, cnt, co);
    }

    // --- ID3D12GraphicsCommandList1 ---
    void STDMETHODCALLTYPE AtomicCopyBufferUINT(ID3D12Resource *dst, UINT64 dstOff, ID3D12Resource *src, UINT64 srcOff,
                                                UINT deps, ID3D12Resource *const *depRes,
                                                const D3D12_SUBRESOURCE_RANGE_UINT64 *depRanges) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList1>())
        {
            c->AtomicCopyBufferUINT(dst, dstOff, src, srcOff, deps, depRes, depRanges);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE AtomicCopyBufferUINT64(ID3D12Resource *dst, UINT64 dstOff, ID3D12Resource *src, UINT64 srcOff,
                                                  UINT deps, ID3D12Resource *const *depRes,
                                                  const D3D12_SUBRESOURCE_RANGE_UINT64 *depRanges) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList1>())
        {
            c->AtomicCopyBufferUINT64(dst, dstOff, src, srcOff, deps, depRes, depRanges);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT mn, FLOAT mx) override
    {
        contState.OnDepthBounds(mn, mx);
        if (auto *c = CurAs<ID3D12GraphicsCommandList1>())
        {
            c->OMSetDepthBounds(mn, mx);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE SetSamplePositions(UINT samplesPerPixel, UINT numPixels,
                                              D3D12_SAMPLE_POSITION *positions) override
    {
        if (ContinuationState::SamplePositionsOverflow(samplesPerPixel, numPixels))
            MarkSplitIneligible("sample_positions");
        else
            contState.OnSamplePositions(samplesPerPixel, numPixels, positions);
        if (auto *c = CurAs<ID3D12GraphicsCommandList1>())
        {
            c->SetSamplePositions(samplesPerPixel, numPixels, positions);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE ResolveSubresourceRegion(ID3D12Resource *dst, UINT dstSub, UINT dstX, UINT dstY,
                                                    ID3D12Resource *src, UINT srcSub, D3D12_RECT *srcRect,
                                                    DXGI_FORMAT fmt, D3D12_RESOLVE_MODE mode) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList1>())
        {
            c->ResolveSubresourceRegion(dst, dstSub, dstX, dstY, src, srcSub, srcRect, fmt, mode);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE SetViewInstanceMask(UINT mask) override
    {
        contState.OnViewInstanceMask(mask);
        if (auto *c = CurAs<ID3D12GraphicsCommandList1>())
        {
            c->SetViewInstanceMask(mask);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList2 ---
    void STDMETHODCALLTYPE WriteBufferImmediate(UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *params,
                                                const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList2>())
        {
            c->WriteBufferImmediate(count, params, modes);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList3 ---
    void STDMETHODCALLTYPE SetProtectedResourceSession(ID3D12ProtectedResourceSession *session) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList3>())
        {
            c->SetProtectedResourceSession(session);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList4 ---
    void STDMETHODCALLTYPE BeginRenderPass(UINT numRTs, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *rts,
                                           const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *ds,
                                           D3D12_RENDER_PASS_FLAGS flags) override
    {
        MarkSplitIneligible("render_pass");
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->BeginRenderPass(numRTs, rts, ds, flags);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE EndRenderPass() override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->EndRenderPass();
            c->Release();
        }
    }
    void STDMETHODCALLTYPE InitializeMetaCommand(ID3D12MetaCommand *cmd, const void *initData, SIZE_T initSize) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->InitializeMetaCommand(cmd, initData, initSize);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE ExecuteMetaCommand(ID3D12MetaCommand *cmd, const void *execData, SIZE_T execSize) override
    {
        MarkSplitIneligible("meta_command");
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->ExecuteMetaCommand(cmd, execData, execSize);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(
        const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc, UINT numPost,
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *post) override
    {
        MarkSplitIneligible("rtas");
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->BuildRaytracingAccelerationStructure(desc, numPost, post);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(
        const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc, UINT numSrc,
        const D3D12_GPU_VIRTUAL_ADDRESS *src) override
    {
        MarkSplitIneligible("rtas");
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->EmitRaytracingAccelerationStructurePostbuildInfo(desc, numSrc, src);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(D3D12_GPU_VIRTUAL_ADDRESS dst,
                                                               D3D12_GPU_VIRTUAL_ADDRESS src,
                                                               D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) override
    {
        MarkSplitIneligible("rtas");
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->CopyRaytracingAccelerationStructure(dst, src, mode);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject *stateObject) override
    {
        MarkSplitIneligible("state_object");
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->SetPipelineState1(stateObject);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC *desc) override
    {
        MarkSplitIneligible("dispatch_rays");
        if (auto *c = CurAs<ID3D12GraphicsCommandList4>())
        {
            c->DispatchRays(desc);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList5 ---
    void STDMETHODCALLTYPE RSSetShadingRate(D3D12_SHADING_RATE base,
                                            const D3D12_SHADING_RATE_COMBINER *combiners) override
    {
        contState.OnShadingRate(base, combiners);
        if (auto *c = CurAs<ID3D12GraphicsCommandList5>())
        {
            c->RSSetShadingRate(base, combiners);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE RSSetShadingRateImage(ID3D12Resource *image) override
    {
        contState.OnShadingRateImage(image);
        if (auto *c = CurAs<ID3D12GraphicsCommandList5>())
        {
            c->RSSetShadingRateImage(image);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList6 ---
    void STDMETHODCALLTYPE DispatchMesh(UINT x, UINT y, UINT z) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList6>())
        {
            c->DispatchMesh(x, y, z);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList7 ---
    void STDMETHODCALLTYPE Barrier(UINT32 numGroups, const D3D12_BARRIER_GROUP *groups) override
    {
        // Enhanced barriers: fail-closed admission until we can classify groups.
        MarkSplitIneligible("enhanced_barrier");
        if (auto *c = CurAs<ID3D12GraphicsCommandList7>())
        {
            c->Barrier(numGroups, groups);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList8 ---
    void STDMETHODCALLTYPE OMSetFrontAndBackStencilRef(UINT front, UINT back) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList8>())
        {
            c->OMSetFrontAndBackStencilRef(front, back);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList9 ---
    void STDMETHODCALLTYPE RSSetDepthBias(FLOAT bias, FLOAT clamp, FLOAT slope) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList9>())
        {
            c->RSSetDepthBias(bias, clamp, slope);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE IASetIndexBufferStripCutValue(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE value) override
    {
        contState.OnStripCut(value);
        if (auto *c = CurAs<ID3D12GraphicsCommandList9>())
        {
            c->IASetIndexBufferStripCutValue(value);
            c->Release();
        }
    }

    // --- ID3D12GraphicsCommandList10 ---
    void STDMETHODCALLTYPE SetProgram(const D3D12_SET_PROGRAM_DESC *desc) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList10>())
        {
            c->SetProgram(desc);
            c->Release();
        }
    }
    void STDMETHODCALLTYPE DispatchGraph(const D3D12_DISPATCH_GRAPH_DESC *desc) override
    {
        if (auto *c = CurAs<ID3D12GraphicsCommandList10>())
        {
            c->DispatchGraph(desc);
            c->Release();
        }
    }
};
} // namespace DlssNr::Submission
