// FSRDStageTimings regression tests on WARP (CPU tier, no hardware GPU).
//
// Invariants verified:
//   1. Successful completed publication: timestamps recorded on CommandListProxy,
//      actually submitted via proxy->ExecuteOn(queue), retired by GpuBatchTracker,
//      and published to Snapshot after GPU fence completion.
//   2. No CPU reading before fence: in-flight GPU work remains unpublished while
//      fence is blocked; proxy Reset with a NEW allocator while in flight does not
//      lose completion after fence completion.
//   3. Discarded recording: never-submitted recording Reset prior to execution
//      drops cleanly without publishing or mapping readback.
//   4. Replay rejection: re-submitting an already-executed recording (submissions==2)
//      invalidates the sample and never publishes timing.
//   5. Generation toggle: toggling enabled state advances generation and drops
//      in-flight samples recorded under the previous generation.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/gpu_time/FSRDStageTimings_Dx12.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/GpuBatchTracker.h"

using Microsoft::WRL::ComPtr;
namespace S = DlssNr::Submission;
namespace GB = DlssNr::Submission::GpuBatch;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "fsrd_stage_timings:%d: %s\n", __LINE__, #condition); \
    std::exit(1); } } while (false)

int main()
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    CHECK(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    CHECK(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))));

    D3D12_COMMAND_QUEUE_DESC qd = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    CHECK(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))));

    uint64_t clockFreq = 0;
    CHECK(SUCCEEDED(queue->GetTimestampFrequency(&clockFreq)) && clockFreq > 0);

    auto makeAllocator = [&]() {
        ComPtr<ID3D12CommandAllocator> alloc;
        CHECK(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
        return alloc;
    };
    auto makeBlockFence = [&]() {
        ComPtr<ID3D12Fence> f;
        CHECK(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f))));
        return f;
    };
    auto makeProxy = [&](ComPtr<ID3D12CommandAllocator>& alloc) {
        ComPtr<ID3D12GraphicsCommandList> real;
        CHECK(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                 IID_PPV_ARGS(&real))));
        ComPtr<S::CommandListProxy> proxy;
        CHECK(SUCCEEDED(S::CommandListProxy::Create(device.Get(), alloc.Get(), real.Get(), &proxy)));
        return proxy;
    };
    auto drainQueueBounded = [&](ID3D12CommandQueue* q) {
        ComPtr<ID3D12Fence> fence = makeBlockFence();
        CHECK(SUCCEEDED(q->Signal(fence.Get(), 1)));
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        CHECK(event != nullptr);
        if (fence->GetCompletedValue() < 1)
        {
            CHECK(SUCCEEDED(fence->SetEventOnCompletion(1, event)));
            CHECK(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0);
        }
        CloseHandle(event);
    };

    auto& tracker = GB::Tracker::Instance();

    // ---- Case 1: Successful completed publication ----
    {
        FSRDStageTimings timer;
        ComPtr<ID3D12CommandAllocator> alloc = makeAllocator();
        ComPtr<S::CommandListProxy> proxy = makeProxy(alloc);

        timer.BeginFrame(device.Get(), proxy.Get(), true);
        CHECK(timer.Begin(FSRDStageTimings::Floor));
        CHECK(timer.Begin(FSRDStageTimings::RayRegeneration));
        timer.End(FSRDStageTimings::Floor);
        timer.End(FSRDStageTimings::RayRegeneration);
        timer.FinishFrame(true);

        CHECK(SUCCEEDED(proxy->Close()));
        // proxy->ExecuteOn submits commands to queue and invokes observer->BeforeExecute (setting clock & submissions=1)
        CHECK(SUCCEEDED(proxy->ExecuteOn(queue.Get())));
        ID3D12CommandList* lists[] = { proxy.Get() };
        tracker.OnExecuted(1, lists, queue.Get());

        // Bounded queue drain and tracker retirement
        drainQueueBounded(queue.Get());
        tracker.OnExecuted(0, nullptr, queue.Get());

        // Snapshot is now published
        timer.BeginFrame(device.Get(), nullptr, true); // drives Collect()
        const auto snap = timer.GetSnapshot();
        CHECK(snap.sequence == 1);
        CHECK((snap.validMask & (1u << FSRDStageTimings::Floor)) != 0);
        CHECK((snap.validMask & (1u << FSRDStageTimings::RayRegeneration)) != 0);
        CHECK((snap.validMask & (1u << FSRDStageTimings::Composition)) == 0);
        CHECK(snap.milliseconds[FSRDStageTimings::Floor] >= 0.0);
        CHECK(snap.milliseconds[FSRDStageTimings::RayRegeneration] >= 0.0);
    }

    // ---- Case 2: No CPU reading before fence; Reset retention while in flight ----
    {
        FSRDStageTimings timer;
        ComPtr<ID3D12CommandAllocator> alloc = makeAllocator();
        ComPtr<S::CommandListProxy> proxy = makeProxy(alloc);

        timer.BeginFrame(device.Get(), proxy.Get(), true);
        CHECK(timer.Begin(FSRDStageTimings::Composition));
        timer.End(FSRDStageTimings::Composition);
        timer.FinishFrame(true);
        CHECK(SUCCEEDED(proxy->Close()));

        // Block queue execution with fence wait
        ComPtr<ID3D12Fence> block = makeBlockFence();
        CHECK(SUCCEEDED(queue->Wait(block.Get(), 1)));

        // Submit via proxy to queue; work queued, fence blocked
        CHECK(SUCCEEDED(proxy->ExecuteOn(queue.Get())));
        ID3D12CommandList* lists[] = { proxy.Get() };
        tracker.OnExecuted(1, lists, queue.Get());

        // While GPU is blocked, CPU must not map or publish snapshot
        timer.BeginFrame(device.Get(), nullptr, true);
        CHECK(timer.GetSnapshot().validMask == 0);

        // Reset proxy while execution is still in-flight using a NEW allocator (old allocator remains in flight)
        ComPtr<ID3D12CommandAllocator> newAlloc = makeAllocator();
        CHECK(SUCCEEDED(proxy->Reset(newAlloc.Get(), nullptr)));
        timer.BeginFrame(device.Get(), nullptr, true);
        CHECK(timer.GetSnapshot().validMask == 0);

        // Release block and drain queue
        CHECK(SUCCEEDED(block->Signal(1)));
        drainQueueBounded(queue.Get());

        tracker.OnExecuted(0, nullptr, queue.Get());
        timer.BeginFrame(device.Get(), nullptr, true); // Collect() promotes completed
        CHECK((timer.GetSnapshot().validMask & (1u << FSRDStageTimings::Composition)) != 0);
    }

    // ---- Case 3: Discarded recording never publishes ----
    {
        FSRDStageTimings timer;
        ComPtr<ID3D12CommandAllocator> alloc = makeAllocator();
        ComPtr<S::CommandListProxy> proxy = makeProxy(alloc);

        timer.BeginFrame(device.Get(), proxy.Get(), true);
        CHECK(timer.Begin(FSRDStageTimings::Conversion));
        timer.End(FSRDStageTimings::Conversion);
        timer.FinishFrame(true);
        CHECK(SUCCEEDED(proxy->Close()));

        // Reset before ever submitting: discards recording
        ComPtr<ID3D12CommandAllocator> newAlloc = makeAllocator();
        CHECK(SUCCEEDED(proxy->Reset(newAlloc.Get(), nullptr)));

        timer.BeginFrame(device.Get(), nullptr, true); // Collect() processes discarded
        CHECK(timer.GetSnapshot().validMask == 0);
    }

    // ---- Case 4: Replay rejected (submissions == 2) ----
    {
        FSRDStageTimings timer;
        ComPtr<ID3D12CommandAllocator> alloc = makeAllocator();
        ComPtr<S::CommandListProxy> proxy = makeProxy(alloc);

        timer.BeginFrame(device.Get(), proxy.Get(), true);
        CHECK(timer.Begin(FSRDStageTimings::AlbedoRecovery));
        timer.End(FSRDStageTimings::AlbedoRecovery);
        timer.FinishFrame(true);
        CHECK(SUCCEEDED(proxy->Close()));

        // Two executions of the closed recording: replay sets submissions=2
        CHECK(SUCCEEDED(proxy->ExecuteOn(queue.Get())));
        CHECK(SUCCEEDED(proxy->ExecuteOn(queue.Get())));
        ID3D12CommandList* lists[] = { proxy.Get() };
        tracker.OnExecuted(1, lists, queue.Get());
        tracker.OnExecuted(1, lists, queue.Get());

        drainQueueBounded(queue.Get());
        tracker.OnExecuted(0, nullptr, queue.Get());

        timer.BeginFrame(device.Get(), nullptr, true);
        // Replayed recording (submissions == 2) was rejected by Publish()
        CHECK(timer.GetSnapshot().validMask == 0);
    }

    // ---- Case 5: Toggled generation drops in-flight sample ----
    {
        FSRDStageTimings timer;
        ComPtr<ID3D12CommandAllocator> alloc = makeAllocator();
        ComPtr<S::CommandListProxy> proxy = makeProxy(alloc);

        timer.BeginFrame(device.Get(), proxy.Get(), true); // Gen 0
        CHECK(timer.Begin(FSRDStageTimings::SuperResolution));
        timer.End(FSRDStageTimings::SuperResolution);
        timer.FinishFrame(true);
        CHECK(SUCCEEDED(proxy->Close()));

        // Block queue
        ComPtr<ID3D12Fence> block = makeBlockFence();
        CHECK(SUCCEEDED(queue->Wait(block.Get(), 1)));

        CHECK(SUCCEEDED(proxy->ExecuteOn(queue.Get())));
        ID3D12CommandList* lists[] = { proxy.Get() };
        tracker.OnExecuted(1, lists, queue.Get());

        // Toggle enabled false then true -> advances generation
        timer.BeginFrame(device.Get(), nullptr, false);
        timer.BeginFrame(device.Get(), nullptr, true);

        // Unblock queue and drain
        CHECK(SUCCEEDED(block->Signal(1)));
        drainQueueBounded(queue.Get());

        tracker.OnExecuted(0, nullptr, queue.Get());
        timer.BeginFrame(device.Get(), nullptr, true);
        // Old generation sample was dropped
        CHECK((timer.GetSnapshot().validMask & (1u << FSRDStageTimings::SuperResolution)) == 0);
    }

    std::puts("fsrd_stage_timings: PASS (WARP D3D12 timestamp queries, proxy lifecycle, replay and generation invariants)");
    return 0;
}
