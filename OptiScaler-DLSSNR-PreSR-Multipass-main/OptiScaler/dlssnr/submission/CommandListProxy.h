#pragma once
#include "LogicalList.h"
#include <atomic>

// Experimental COM proxy for ID3D12GraphicsCommandList (base only).
// QueryInterface for ID3D12GraphicsCommandList1..10 returns E_NOINTERFACE (fail-closed).
namespace DlssNr::Submission
{
MIDL_INTERFACE("b3c0e9a1-4d2f-4c77-9a18-6f2d8e1b4c01")
ILogicalCommandList : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE SplitSegments(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExecuteOn(ID3D12CommandQueue *queue) = 0;
};

class CommandListProxy final : public ID3D12GraphicsCommandList, public ILogicalCommandList
{
    std::atomic<ULONG> refs { 1 };
    LogicalList logical;

    ID3D12GraphicsCommandList *Cur() const { return logical.Current(); }

  public:
    static HRESULT Create(ID3D12Device *device, ID3D12CommandAllocator *alloc, ID3D12GraphicsCommandList *real,
                          CommandListProxy **out)
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
        *out = p;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (!ppv)
            return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == __uuidof(ID3D12Object) || riid == __uuidof(ID3D12DeviceChild) ||
            riid == __uuidof(ID3D12CommandList) || riid == __uuidof(ID3D12GraphicsCommandList))
        {
            *ppv = static_cast<ID3D12GraphicsCommandList *>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(ILogicalCommandList))
        {
            *ppv = static_cast<ILogicalCommandList *>(this);
            AddRef();
            return S_OK;
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
        return logical.Reset(alloc, initial);
    }
    HRESULT STDMETHODCALLTYPE SplitSegments() override { return logical.Split(); }
    HRESULT STDMETHODCALLTYPE ExecuteOn(ID3D12CommandQueue *queue) override { return logical.Execute(queue); }

    void STDMETHODCALLTYPE ClearState(ID3D12PipelineState *p) override
    {
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
        if (auto *c = Cur())
            c->IASetPrimitiveTopology(t);
    }
    void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D12_VIEWPORT *v) override
    {
        if (auto *c = Cur())
            c->RSSetViewports(n, v);
    }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D12_RECT *r) override
    {
        if (auto *c = Cur())
            c->RSSetScissorRects(n, r);
    }
    void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT f[4]) override
    {
        if (auto *c = Cur())
            c->OMSetBlendFactor(f);
    }
    void STDMETHODCALLTYPE OMSetStencilRef(UINT s) override
    {
        if (auto *c = Cur())
            c->OMSetStencilRef(s);
    }
    void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState *p) override
    {
        if (auto *c = Cur())
            c->SetPipelineState(p);
    }
    void STDMETHODCALLTYPE ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER *b) override
    {
        if (auto *c = Cur())
            c->ResourceBarrier(n, b);
    }
    void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *l) override
    {
        if (auto *c = Cur())
            c->ExecuteBundle(l);
    }
    void STDMETHODCALLTYPE SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap *const *h) override
    {
        if (auto *c = Cur())
            c->SetDescriptorHeaps(n, h);
    }
    void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature *s) override
    {
        if (auto *c = Cur())
            c->SetComputeRootSignature(s);
    }
    void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *s) override
    {
        if (auto *c = Cur())
            c->SetGraphicsRootSignature(s);
    }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) override
    {
        if (auto *c = Cur())
            c->SetComputeRootDescriptorTable(i, h);
    }
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) override
    {
        if (auto *c = Cur())
            c->SetGraphicsRootDescriptorTable(i, h);
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT i, UINT v, UINT o) override
    {
        if (auto *c = Cur())
            c->SetComputeRoot32BitConstant(i, v, o);
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT i, UINT v, UINT o) override
    {
        if (auto *c = Cur())
            c->SetGraphicsRoot32BitConstant(i, v, o);
    }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT i, UINT n, const void *s, UINT o) override
    {
        if (auto *c = Cur())
            c->SetComputeRoot32BitConstants(i, n, s, o);
    }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT i, UINT n, const void *s, UINT o) override
    {
        if (auto *c = Cur())
            c->SetGraphicsRoot32BitConstants(i, n, s, o);
    }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (auto *c = Cur())
            c->SetComputeRootConstantBufferView(i, a);
    }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (auto *c = Cur())
            c->SetGraphicsRootConstantBufferView(i, a);
    }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (auto *c = Cur())
            c->SetComputeRootShaderResourceView(i, a);
    }
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (auto *c = Cur())
            c->SetGraphicsRootShaderResourceView(i, a);
    }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (auto *c = Cur())
            c->SetComputeRootUnorderedAccessView(i, a);
    }
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS a) override
    {
        if (auto *c = Cur())
            c->SetGraphicsRootUnorderedAccessView(i, a);
    }
    void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *v) override
    {
        if (auto *c = Cur())
            c->IASetIndexBuffer(v);
    }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT s, UINT n, const D3D12_VERTEX_BUFFER_VIEW *v) override
    {
        if (auto *c = Cur())
            c->IASetVertexBuffers(s, n, v);
    }
    void STDMETHODCALLTYPE SOSetTargets(UINT s, UINT n, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *v) override
    {
        if (auto *c = Cur())
            c->SOSetTargets(s, n, v);
    }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *rt, BOOL single,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE *ds) override
    {
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
        if (auto *c = Cur())
            c->BeginQuery(h, t, i);
    }
    void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *h, D3D12_QUERY_TYPE t, UINT i) override
    {
        if (auto *c = Cur())
            c->EndQuery(h, t, i);
    }
    void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *h, D3D12_QUERY_TYPE t, UINT s, UINT n, ID3D12Resource *d,
                                            UINT64 o) override
    {
        if (auto *c = Cur())
            c->ResolveQueryData(h, t, s, n, d, o);
    }
    void STDMETHODCALLTYPE SetPredication(ID3D12Resource *b, UINT64 o, D3D12_PREDICATION_OP op) override
    {
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
};
} // namespace DlssNr::Submission
