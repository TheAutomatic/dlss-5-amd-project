#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/RecordingGpuTiming.h"
using Microsoft::WRL::ComPtr;
static void Require(bool value, const char* what) { if (!value) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); } }
static void Check(HRESULT hr, const char* what) { Require(SUCCEEDED(hr), what); }
int main()
{
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter; ComPtr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP");
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd {};
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    ComPtr<ID3D12CommandAllocator> allocator, allocator2; ComPtr<ID3D12GraphicsCommandList> list, list2;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator2)), "allocator2");
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator2.Get(), nullptr, IID_PPV_ARGS(&list2)), "list2");
    LmxxfRuntime::RecordingGpuTimingPool pool;
    std::vector<std::shared_ptr<LmxxfRuntime::RecordingGpuTiming>> held;
    for (unsigned i = 0; i < 16; ++i) {
        auto item = pool.Acquire(device.Get()); Require(bool(item), "pool allocation"); held.push_back(item);
    }
    Require(!pool.Acquire(device.Get()), "live recordings exhaust pool without overwriting");
    auto* reusable = held[5].get(); held[5].reset();
    auto reused = pool.Acquire(device.Get()); Require(reused.get() == reusable, "released recording storage is reused");
    held.clear(); reused.reset();
    auto timing = pool.Acquire(device.Get()); Require(bool(timing), "timing resources");
    timing->Begin(list.Get(), 0); timing->End(list.Get(), 0);
    timing->Begin(list.Get(), 1); timing->End(list.Get(), 1);
    Check(list->Close(), "close");
    timing->Begin(list2.Get(), 0); timing->End(list2.Get(), 0);
    timing->Begin(list2.Get(), 1); timing->End(list2.Get(), 1);
    Check(list2->Close(), "close list2");
    ComPtr<ID3D12Fence> fence, gate;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
    DlssNr::PerformanceStore store; store.SetEnabled(true);
    uint64_t value = 0;
    auto submit = [&](ID3D12GraphicsCommandList* cl = nullptr) {
        if (!cl) cl = list.Get();
        timing->BeforeExecution(store);
        ID3D12CommandList* lists[] = {cl}; queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.Get(), ++value), "signal");
        timing->Submitted(std::make_shared<LmxxfRuntime::RecordingCompletion>(fence.Get(), queue.Get(), value),
                          true, 42, value, store.Epoch());
    };
    auto wait = [&] {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Require(event != nullptr, "event");
        Check(fence->SetEventOnCompletion(value, event), "completion event");
        Require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "bounded test completion"); CloseHandle(event);
        timing->Collect(store);
    };
    timing->Collect(store); Require(!store.Read().stages[NR_GPU_ENCODE].samples, "unsubmitted has no samples");
    Check(queue->Wait(gate.Get(), 1), "hold queue"); submit(); timing->Collect(store);
    Require(!store.Read().stages[NR_GPU_ENCODE].samples, "pending query is not read");
    Check(gate->Signal(1), "release queue"); wait();
    auto first = store.Read();
    Require(first.stages[NR_GPU_ENCODE].samples == 1 && first.stages[NR_GPU_DECODE].samples == 1, "both completed stages");
    timing->Collect(store); Require(store.Read().stages[NR_GPU_ENCODE].samples == 1, "collect once");
    Check(queue->Wait(gate.Get(), 2), "hold replay"); submit(list.Get()); submit(list2.Get());
    Require(store.Read().dropped == 1, "in-flight replay drops old readback sample");
    Check(gate->Signal(2), "release replay"); wait();
    Require(store.Read().stages[NR_GPU_ENCODE].samples == 2 && store.Read().stages[NR_GPU_DECODE].execution_id == value,
            "last replay has correct identity");
    submit(); store.SetEnabled(false); store.SetEnabled(true); wait();
    Require(!store.Read().stages[NR_GPU_ENCODE].samples, "old enable epoch cannot repopulate stats");
    submit(); wait(); Require(store.Read().stages[NR_GPU_ENCODE].samples == 1, "new enable epoch records");
    ComPtr<ID3D12InfoQueue> info;
    if (SUCCEEDED(device.As(&info))) for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0; info->GetMessage(i, nullptr, &bytes); std::vector<char> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data()); Check(info->GetMessage(i, message, &bytes), "message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) std::fprintf(stderr, "%s\n", message->pDescription);
        Require(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, "debug layer");
    }
    std::puts("recording GPU timing: PASS (pending, replay, epoch, WARP timestamps)");
}
