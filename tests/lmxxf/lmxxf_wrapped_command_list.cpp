#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/AwaitingListTracker.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/SubmissionHooks.h"

using Microsoft::WRL::ComPtr;
namespace S = DlssNr::Submission;
static void Require(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static void Check(HRESULT hr, const char* message) { Require(SUCCEEDED(hr), message); }

// Model only the COM boundary used by QueryLogicalCommandList plus the graphics
// command surface an outer layer (ReShade/RenoDX, Streamline interposer) shows
// the game: its own identity, unknown IIDs forwarded to its original object,
// and CopyResource/CopyBufferRegion/Dispatch counted while traversing it.
// GPU work below uses the real production proxy and WARP, not a fake list.
class ForwardingLayer final : public ID3D12GraphicsCommandList
{
    ULONG refs = 1;
    ComPtr<ID3D12GraphicsCommandList> original;
    bool forward;
    bool nativeOnly;
public:
    inline static unsigned live = 0;
    unsigned copies = 0, dispatches = 0;
    ForwardingLayer(ID3D12GraphicsCommandList* wrapped, bool enabled = true, bool native = false)
        : original(wrapped), forward(enabled), nativeOnly(native) { ++live; }
    ~ForwardingLayer() { --live; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(ID3D12Object) || iid == __uuidof(ID3D12DeviceChild) ||
            iid == __uuidof(ID3D12CommandList) || iid == __uuidof(ID3D12GraphicsCommandList))
        {
            *out = static_cast<ID3D12GraphicsCommandList*>(this);
            AddRef();
            return S_OK;
        }
        if (nativeOnly) {
            static constexpr GUID nativeIid = {0xADEC44E2, 0x61F0, 0x45C3,
                {0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF}};
            if (iid != nativeIid) return E_NOINTERFACE;
            *out = original.Get(); original->AddRef(); return S_OK;
        }
        return forward ? original->QueryInterface(iid, out) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --refs; if (!count) delete this; return count; }

    HRESULT STDMETHODCALLTYPE GetPrivateData(_In_ REFGUID guid, _Inout_ UINT *pDataSize, _Out_writes_bytes_opt_( *pDataSize ) void *pData) override { return original->GetPrivateData(guid, pDataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(_In_ REFGUID guid, _In_ UINT DataSize, _In_reads_bytes_opt_( DataSize ) const void *pData) override { return original->SetPrivateData(guid, DataSize, pData); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(_In_ REFGUID guid, _In_opt_ const IUnknown *pData) override { return original->SetPrivateDataInterface(guid, pData); }
    HRESULT STDMETHODCALLTYPE SetName(_In_z_ LPCWSTR Name) override { return original->SetName(Name); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, _COM_Outptr_opt_ void **ppvDevice) override { return original->GetDevice(riid, ppvDevice); }
    D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return original->GetType(); }
    HRESULT STDMETHODCALLTYPE Close() override { return original->Close(); }
    HRESULT STDMETHODCALLTYPE Reset(_In_ ID3D12CommandAllocator *pAllocator, _In_opt_ ID3D12PipelineState *pInitialState) override { return original->Reset(pAllocator, pInitialState); }
    void STDMETHODCALLTYPE ClearState(_In_opt_ ID3D12PipelineState *pPipelineState) override { original->ClearState(pPipelineState); }
    void STDMETHODCALLTYPE DrawInstanced(_In_ UINT VertexCountPerInstance, _In_ UINT InstanceCount, _In_ UINT StartVertexLocation, _In_ UINT StartInstanceLocation) override { original->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation); }
    void STDMETHODCALLTYPE DrawIndexedInstanced(_In_ UINT IndexCountPerInstance, _In_ UINT InstanceCount, _In_ UINT StartIndexLocation, _In_ INT BaseVertexLocation, _In_ UINT StartInstanceLocation) override { original->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation); }
    void STDMETHODCALLTYPE Dispatch(_In_ UINT ThreadGroupCountX, _In_ UINT ThreadGroupCountY, _In_ UINT ThreadGroupCountZ) override { ++dispatches; original->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ); }
    void STDMETHODCALLTYPE CopyBufferRegion(_In_ ID3D12Resource *pDstBuffer, UINT64 DstOffset, _In_ ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes) override { ++copies; original->CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes); }
    void STDMETHODCALLTYPE CopyTextureRegion(_In_ const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, _In_ const D3D12_TEXTURE_COPY_LOCATION *pSrc, _In_opt_ const D3D12_BOX *pSrcBox) override { original->CopyTextureRegion(pDst, DstX, DstY, DstZ, pSrc, pSrcBox); }
    void STDMETHODCALLTYPE CopyResource(_In_ ID3D12Resource *pDstResource, _In_ ID3D12Resource *pSrcResource) override { ++copies; original->CopyResource(pDstResource, pSrcResource); }
    void STDMETHODCALLTYPE CopyTiles(_In_ ID3D12Resource *pTiledResource, _In_ const D3D12_TILED_RESOURCE_COORDINATE *pTileRegionStartCoordinate, _In_ const D3D12_TILE_REGION_SIZE *pTileRegionSize, _In_ ID3D12Resource *pBuffer, UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags) override { original->CopyTiles(pTiledResource, pTileRegionStartCoordinate, pTileRegionSize, pBuffer, BufferStartOffsetInBytes, Flags); }
    void STDMETHODCALLTYPE ResolveSubresource(_In_ ID3D12Resource *pDstResource, _In_ UINT DstSubresource, _In_ ID3D12Resource *pSrcResource, _In_ UINT SrcSubresource, _In_ DXGI_FORMAT Format) override { original->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format); }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(_In_ D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology) override { original->IASetPrimitiveTopology(PrimitiveTopology); }
    void STDMETHODCALLTYPE RSSetViewports(_In_range_(0, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) UINT NumViewports, _In_reads_( NumViewports) const D3D12_VIEWPORT *pViewports) override { original->RSSetViewports(NumViewports, pViewports); }
    void STDMETHODCALLTYPE RSSetScissorRects(_In_range_(0, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) UINT NumRects, _In_reads_( NumRects) const D3D12_RECT *pRects) override { original->RSSetScissorRects(NumRects, pRects); }
    void STDMETHODCALLTYPE OMSetBlendFactor(_In_reads_opt_(4) const FLOAT BlendFactor[ 4 ]) override { original->OMSetBlendFactor(BlendFactor); }
    void STDMETHODCALLTYPE OMSetStencilRef(_In_ UINT StencilRef) override { original->OMSetStencilRef(StencilRef); }
    void STDMETHODCALLTYPE SetPipelineState(_In_ ID3D12PipelineState *pPipelineState) override { original->SetPipelineState(pPipelineState); }
    void STDMETHODCALLTYPE ResourceBarrier(_In_ UINT NumBarriers, _In_reads_(NumBarriers) const D3D12_RESOURCE_BARRIER *pBarriers) override { original->ResourceBarrier(NumBarriers, pBarriers); }
    void STDMETHODCALLTYPE ExecuteBundle(_In_ ID3D12GraphicsCommandList *pCommandList) override { original->ExecuteBundle(pCommandList); }
    void STDMETHODCALLTYPE SetDescriptorHeaps(_In_ UINT NumDescriptorHeaps, _In_reads_(NumDescriptorHeaps) ID3D12DescriptorHeap *const *ppDescriptorHeaps) override { original->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps); }
    void STDMETHODCALLTYPE SetComputeRootSignature(_In_opt_ ID3D12RootSignature *pRootSignature) override { original->SetComputeRootSignature(pRootSignature); }
    void STDMETHODCALLTYPE SetGraphicsRootSignature(_In_opt_ ID3D12RootSignature *pRootSignature) override { original->SetGraphicsRootSignature(pRootSignature); }
    void STDMETHODCALLTYPE SetComputeRootDescriptorTable(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override { original->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor); }
    void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor) override { original->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor); }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstant(_In_ UINT RootParameterIndex, _In_ UINT SrcData, _In_ UINT DestOffsetIn32BitValues) override { original->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(_In_ UINT RootParameterIndex, _In_ UINT SrcData, _In_ UINT DestOffsetIn32BitValues) override { original->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetComputeRoot32BitConstants(_In_ UINT RootParameterIndex, _In_ UINT Num32BitValuesToSet, _In_reads_(Num32BitValuesToSet*sizeof(UINT)) const void *pSrcData, _In_ UINT DestOffsetIn32BitValues) override { original->SetComputeRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(_In_ UINT RootParameterIndex, _In_ UINT Num32BitValuesToSet, _In_reads_(Num32BitValuesToSet*sizeof(UINT)) const void *pSrcData, _In_ UINT DestOffsetIn32BitValues) override { original->SetGraphicsRoot32BitConstants(RootParameterIndex, Num32BitValuesToSet, pSrcData, DestOffsetIn32BitValues); }
    void STDMETHODCALLTYPE SetComputeRootConstantBufferView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { original->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { original->SetGraphicsRootConstantBufferView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetComputeRootShaderResourceView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { original->SetComputeRootShaderResourceView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { original->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { original->SetComputeRootUnorderedAccessView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(_In_ UINT RootParameterIndex, _In_ D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override { original->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BufferLocation); }
    void STDMETHODCALLTYPE IASetIndexBuffer(_In_opt_ const D3D12_INDEX_BUFFER_VIEW *pView) override { original->IASetIndexBuffer(pView); }
    void STDMETHODCALLTYPE IASetVertexBuffers(_In_ UINT StartSlot, _In_ UINT NumViews, _In_reads_opt_(NumViews) const D3D12_VERTEX_BUFFER_VIEW *pViews) override { original->IASetVertexBuffers(StartSlot, NumViews, pViews); }
    void STDMETHODCALLTYPE SOSetTargets(_In_ UINT StartSlot, _In_ UINT NumViews, _In_reads_opt_(NumViews) const D3D12_STREAM_OUTPUT_BUFFER_VIEW *pViews) override { original->SOSetTargets(StartSlot, NumViews, pViews); }
    void STDMETHODCALLTYPE OMSetRenderTargets(_In_ UINT NumRenderTargetDescriptors, _In_opt_ const D3D12_CPU_DESCRIPTOR_HANDLE *pRenderTargetDescriptors, _In_ BOOL RTsSingleHandleToDescriptorRange, _In_opt_ const D3D12_CPU_DESCRIPTOR_HANDLE *pDepthStencilDescriptor) override { original->OMSetRenderTargets(NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor); }
    void STDMETHODCALLTYPE ClearDepthStencilView(_In_ D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView, _In_ D3D12_CLEAR_FLAGS ClearFlags, _In_ FLOAT Depth, _In_ UINT8 Stencil, _In_ UINT NumRects, _In_reads_(NumRects) const D3D12_RECT *pRects) override { original->ClearDepthStencilView(DepthStencilView, ClearFlags, Depth, Stencil, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearRenderTargetView(_In_ D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView, _In_ const FLOAT ColorRGBA[ 4 ], _In_ UINT NumRects, _In_reads_(NumRects) const D3D12_RECT *pRects) override { original->ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(_In_ D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, _In_ D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, _In_ ID3D12Resource *pResource, _In_ const UINT Values[ 4 ], _In_ UINT NumRects, _In_reads_(NumRects) const D3D12_RECT *pRects) override { original->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(_In_ D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, _In_ D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, _In_ ID3D12Resource *pResource, _In_ const FLOAT Values[ 4 ], _In_ UINT NumRects, _In_reads_(NumRects) const D3D12_RECT *pRects) override { original->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects); }
    void STDMETHODCALLTYPE DiscardResource(_In_ ID3D12Resource *pResource, _In_opt_ const D3D12_DISCARD_REGION *pRegion) override { original->DiscardResource(pResource, pRegion); }
    void STDMETHODCALLTYPE BeginQuery(_In_ ID3D12QueryHeap *pQueryHeap, _In_ D3D12_QUERY_TYPE Type, _In_ UINT Index) override { original->BeginQuery(pQueryHeap, Type, Index); }
    void STDMETHODCALLTYPE EndQuery(_In_ ID3D12QueryHeap *pQueryHeap, _In_ D3D12_QUERY_TYPE Type, _In_ UINT Index) override { original->EndQuery(pQueryHeap, Type, Index); }
    void STDMETHODCALLTYPE ResolveQueryData(_In_ ID3D12QueryHeap *pQueryHeap, _In_ D3D12_QUERY_TYPE Type, _In_ UINT StartIndex, _In_ UINT NumQueries, _In_ ID3D12Resource *pDestinationBuffer, _In_ UINT64 AlignedDestinationBufferOffset) override { original->ResolveQueryData(pQueryHeap, Type, StartIndex, NumQueries, pDestinationBuffer, AlignedDestinationBufferOffset); }
    void STDMETHODCALLTYPE SetPredication(_In_opt_ ID3D12Resource *pBuffer, _In_ UINT64 AlignedBufferOffset, _In_ D3D12_PREDICATION_OP Operation) override { original->SetPredication(pBuffer, AlignedBufferOffset, Operation); }
    void STDMETHODCALLTYPE SetMarker(UINT Metadata, _In_reads_bytes_opt_(Size) const void *pData, UINT Size) override { original->SetMarker(Metadata, pData, Size); }
    void STDMETHODCALLTYPE BeginEvent(UINT Metadata, _In_reads_bytes_opt_(Size) const void *pData, UINT Size) override { original->BeginEvent(Metadata, pData, Size); }
    void STDMETHODCALLTYPE EndEvent() override { original->EndEvent(); }
    void STDMETHODCALLTYPE ExecuteIndirect(_In_ ID3D12CommandSignature *pCommandSignature, _In_ UINT MaxCommandCount, _In_ ID3D12Resource *pArgumentBuffer, _In_ UINT64 ArgumentBufferOffset, _In_opt_ ID3D12Resource *pCountBuffer, _In_ UINT64 CountBufferOffset) override { original->ExecuteIndirect(pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset); }
};

struct Observer final : S::RecordingObserver
{
    unsigned producers = 0, between = 0, executions = 0, invalidations = 0;
    void ProducerSubmitted(const S::RecordingExecution& e) noexcept override
    {
        Require(e.producerSubmitted && !e.continuationSubmitted, "producer precedes continuation");
        ++producers;
    }
    void Between(const S::RecordingExecution& e) noexcept override
    {
        Require(e.producerSubmitted && !e.continuationSubmitted, "NR slot stays between segments");
        ++between;
    }
    void Executed(const S::RecordingExecution& e) noexcept override
    {
        Require(SUCCEEDED(e.status) && e.continuationSubmitted, "complete split execution");
        ++executions;
    }
    void Invalidated(S::RecordingIdentity) noexcept override { ++invalidations; }
};

static ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp {}; hp.Type = heap;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = 256;
    desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                         IID_PPV_ARGS(&result)), "buffer");
    return result;
}
static void Wait(ID3D12Device* device, ID3D12CommandQueue* queue)
{
    ComPtr<ID3D12Fence> fence;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    Check(queue->Signal(fence.Get(), 1), "signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "event");
    Check(fence->SetEventOnCompletion(1, event), "completion event");
    Require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "completion timeout");
    CloseHandle(event);
    Require(fence->GetCompletedValue() != UINT64_MAX, "fence is not device loss");
    Check(device->GetDeviceRemovedReason(), "device healthy");
}
static void WINAPI ExecuteNative(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    for (UINT i = 0; i < count; ++i)
        Require(!S::QueryLogicalCommandList(lists[i]), "raw queue receives only native lists");
    queue->ExecuteCommandLists(count, lists);
}

static void Run(ID3D12Device* device, ID3D12CommandQueue* queue, unsigned layers, bool split, bool nativeOnly = false)
{
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> native;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "allocator");
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                    IID_PPV_ARGS(&native)), "native list");
    Require(!S::QueryLogicalCommandList(native.Get()), "ordinary lists remain unchanged");
    {
        ComPtr<IUnknown> foreign;
        foreign.Attach(new ForwardingLayer(native.Get(), true, nativeOnly));
        Require(!S::QueryLogicalCommandList(foreign.Get()), "wrapper without our proxy remains unchanged");
    }
    ComPtr<S::CommandListProxy> proxy;
    Check(S::CommandListProxy::Create(device, alloc.Get(), native.Get(), proxy.GetAddressOf()), "proxy");
    ComPtr<ID3D12GraphicsCommandList> outer = static_cast<ID3D12GraphicsCommandList*>(proxy.Get());
    ForwardingLayer* outerLayer = nullptr;
    for (unsigned i = 0; i < layers; ++i)
    {
        outerLayer = new ForwardingLayer(outer.Get(), true, nativeOnly);
        outer.Attach(outerLayer);
    }
    {
        ComPtr<IUnknown> opaque;
        opaque.Attach(new ForwardingLayer(outer.Get(), false));
        Require(!S::QueryLogicalCommandList(opaque.Get()), "non-forwarding wrapper remains unchanged");
    }
    const auto baselineRefs = proxy->AddRef(); proxy->Release();
    for (unsigned i = 0; i < 32; ++i)
    {
        auto earlyReturn = S::QueryLogicalCommandList(outer.Get());
        Require(earlyReturn.Get() == proxy.Get(), "query reaches proxy, never the native producer");
    }
    const auto afterRefs = proxy->AddRef(); proxy->Release();
    Require(baselineRefs == afterRefs, "early returns balance all queried references");

    auto cmd = S::QueryLogicalCommandList(outer.Get());
    if (layers) Require(static_cast<IUnknown*>(cmd.Get()) != outer.Get(), "wrapper and submission identities differ");
    ComPtr<ID3D12Device> listDevice, queueDevice;
    Check(cmd->GetDevice(IID_PPV_ARGS(&listDevice)), "resolved list device");
    Check(queue->GetDevice(IID_PPV_ARGS(&queueDevice)), "queue device");
    ComPtr<IUnknown> listId, queueId;
    Check(listDevice.As(&listId), "list device identity");
    Check(queueDevice.As(&queueId), "queue device identity");
    Require(listId == queueId, "list device matches observed queue device");
    DlssNr::AmdBridge::AwaitingListTracker awaiting;
    awaiting.Add(cmd.Get());
    ID3D12CommandList* submitted[] = {proxy.Get()}; // ReShade submits its original.
    Require(awaiting.MatchAndRemove(1, submitted) == cmd.Get() && awaiting.Count() == 0,
            "queue discovery matches the recovered proxy");

    // NVNGX_DLSS_Dx12 evaluate-time decision, modeled 1:1: a transparent outer
    // layer (ReShade around our proxy) stays on the path so its dispatch and
    // root-bind callbacks keep running; an opaque Streamline-style wrapper is
    // replaced by the recovered proxy.
    bool unwrapOuter = false;
    auto logicalCmd = S::QueryLogicalCommandList(outer.Get(), &unwrapOuter);
    Require(logicalCmd.Get() == proxy.Get(), "evaluate query reaches the proxy");
    if (nativeOnly)
        Require(unwrapOuter, "opaque wrapper requires the outer unwrap");
    else
        Require(!unwrapOuter, "forwarding wrapper must keep the outer path");
    ComPtr<ID3D12GraphicsCommandList> gameList = (logicalCmd && unwrapOuter) ? logicalCmd : outer;

    auto observer = std::make_shared<Observer>();
    if (split) Check(proxy->ObserveRecording(observer), "observe split recording");
    auto upload = Buffer(device, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    auto gpu = Buffer(device, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    auto readback = Buffer(device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    void* mapped = nullptr;
    Check(upload->Map(0, nullptr, &mapped), "upload map");
    std::memset(mapped, 0x30 + layers + (split ? 4 : 0), 256);
    upload->Unmap(0, nullptr);
    cmd->CopyBufferRegion(gpu.Get(), 0, upload.Get(), 0, 256);
    if (split)
    {
        Check(proxy->SplitSegments(), "split");
        auto again = S::QueryLogicalCommandList(outer.Get());
        Require(again.Get() == cmd.Get() && S::GraphicsRecordingList(cmd.Get()) != native.Get(),
                "proxy identity survives producer to continuation switch");
    }
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                         D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
    {
        gameList->ResourceBarrier(1, &barrier);
        gameList->CopyBufferRegion(readback.Get(), 0, gpu.Get(), 0, 256);
        if (outerLayer)
        {
            Require(outerLayer->copies == (nativeOnly ? 0u : 1u),
                    "post-split copy traversal follows the unwrap decision");
            Require(outerLayer->dispatches == 0, "no dispatch recorded in the executed workload");
        }
    }
    gameList.Reset();
    logicalCmd.Reset();
    Check(cmd->Close(), "close");
    // The Evaluate-held reference must own the proxy even if outer references go away.
    outer.Reset(); proxy.Reset();
    Require(ForwardingLayer::live == 0 && observer->invalidations == 0, "only scoped proxy reference remains");
    S::Hooks::ExecuteExpanded(queue, 1, submitted, nullptr, nullptr, &ExecuteNative);
    Wait(device, queue);
    D3D12_RANGE readRange {0, 256};
    Check(readback->Map(0, &readRange, &mapped), "readback map");
    for (unsigned i = 0; i < 256; ++i)
        Require(static_cast<unsigned char*>(mapped)[i] == 0x30 + layers + (split ? 4 : 0),
                "producer data reaches continuation byte-exactly");
    readback->Unmap(0, nullptr);
    if (split) Require(observer->producers == 1 && observer->between == 1 && observer->executions == 1,
                       "exactly one producer, NR slot and continuation");
    cmd.Reset();
    if (split) Require(observer->invalidations == 1, "last scoped reference releases recording exactly once");

    if (layers)
    {
        // Dispatch through the outer layer takes the same path: traversed when
        // transparent, replaced by the proxy when opaque. Recording-only probe
        // on a fresh list that is never submitted.
        ComPtr<ID3D12CommandAllocator> probeAlloc;
        ComPtr<ID3D12GraphicsCommandList> probeNative;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&probeAlloc)),
              "probe allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, probeAlloc.Get(), nullptr,
                                        IID_PPV_ARGS(&probeNative)), "probe list");
        ComPtr<S::CommandListProxy> probeProxy;
        Check(S::CommandListProxy::Create(device, probeAlloc.Get(), probeNative.Get(), probeProxy.GetAddressOf()),
              "probe proxy");
        ComPtr<ForwardingLayer> probeLayer;
        probeLayer.Attach(new ForwardingLayer(static_cast<ID3D12GraphicsCommandList*>(probeProxy.Get()), true, nativeOnly));
        bool probeUnwrap = false;
        const auto probeLogical = S::QueryLogicalCommandList(probeLayer.Get(), &probeUnwrap);
        Require(probeUnwrap == nativeOnly, "probe unwrap decision matches the wrapper kind");
        // Observation-only QI must not poison the proxy: ResTrack asks every tracked list
        // for ID3DDestructionNotifier (OptiFG path), and an escape there would reject all
        // later recordings for the rest of the process (FH6 report 2026-10-25).
        {
            ComPtr<struct ID3DDestructionNotifier> notifier;
            const HRESULT notifierHr = probeProxy->QueryInterface(IID_PPV_ARGS(&notifier));
            Require(SUCCEEDED(notifierHr) && notifier != nullptr,
                    "a D3D12 command list exposes ID3DDestructionNotifier through the proxy");
            notifier.Reset();
            Require(probeProxy->CanRecordInline(),
                    "destruction-notifier QI leaves the proxy recordable");
            Require(std::strcmp(probeProxy->InlineRejectionReason(), "native_interface_escaped") != 0,
                    "destruction-notifier QI is not an escape");
        }
        ID3D12GraphicsCommandList* probeList = (probeLogical && probeUnwrap) ? probeLogical.Get() : probeLayer.Get();
        probeList->Dispatch(1, 1, 1);
        Require(probeLayer->dispatches == (nativeOnly ? 0u : 1u),
                "dispatch traverses the outer wrapper unless replaced");
        Check(probeList->Close(), "probe close");
    }
}

int main()
{
    Require(!S::QueryLogicalCommandList(nullptr), "null list");
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
    Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC desc {};
    Check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue");
    for (unsigned layers = 0; layers <= 2; ++layers)
        for (bool split : {false, true}) Run(device.Get(), queue.Get(), layers, split);
    for (bool split : {false, true}) Run(device.Get(), queue.Get(), 1, split, true);
    std::puts("wrapped command list: PASS (forwarded/native-only identity, unwrap decision, queue discovery, split copies, outer copy/dispatch traversal, scoped references; WARP)");
}
