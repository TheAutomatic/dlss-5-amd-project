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

// Model only the COM-query boundary used by QueryLogicalCommandList: an outer
// layer has its own identity and forwards unknown IIDs to its original object.
// GPU work below uses the real production proxy and WARP, not a fake list.
class ForwardingLayer final : public IUnknown
{
    ULONG refs = 1;
    ComPtr<IUnknown> original;
    bool forward;
public:
    inline static unsigned live = 0;
    ForwardingLayer(IUnknown* wrapped, bool enabled = true) : original(wrapped), forward(enabled) { ++live; }
    ~ForwardingLayer() { --live; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown) { *out = static_cast<IUnknown*>(this); AddRef(); return S_OK; }
        return forward ? original->QueryInterface(iid, out) : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --refs; if (!count) delete this; return count; }
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

static void Run(ID3D12Device* device, ID3D12CommandQueue* queue, unsigned layers, bool split)
{
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> native;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "allocator");
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                    IID_PPV_ARGS(&native)), "native list");
    Require(!S::QueryLogicalCommandList(native.Get()), "ordinary lists remain unchanged");
    {
        ComPtr<IUnknown> foreign;
        foreign.Attach(new ForwardingLayer(native.Get()));
        Require(!S::QueryLogicalCommandList(foreign.Get()), "wrapper without our proxy remains unchanged");
    }
    ComPtr<S::CommandListProxy> proxy;
    Check(S::CommandListProxy::Create(device, alloc.Get(), native.Get(), proxy.GetAddressOf()), "proxy");
    ComPtr<IUnknown> outer = static_cast<ID3D12GraphicsCommandList*>(proxy.Get());
    for (unsigned i = 0; i < layers; ++i) outer.Attach(new ForwardingLayer(outer.Get()));
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
    cmd->ResourceBarrier(1, &barrier);
    cmd->CopyBufferRegion(readback.Get(), 0, gpu.Get(), 0, 256);
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
    std::puts("wrapped command list: PASS (identity, queue discovery, split copies, scoped references; WARP)");
}
