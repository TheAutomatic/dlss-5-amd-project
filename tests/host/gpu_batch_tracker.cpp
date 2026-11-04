// GpuBatchTracker lifecycle semantics on a WARP device (CPU tier, no hardware GPU).
//
// Covers the step-1 lifecycle work:
//   T1 native list: Record -> OnExecuted -> fence retirement
//   T2 native replay: a re-submitted completed list is re-armed Outstanding until the
//      replay's fence completes, and the replay-after-completion counter increments
//   T3 proxy Reset invalidates a never-submitted recording immediately (no GPU wait)
//   T4 proxy Reset does NOT retire a submitted recording while its fence is pending:
//      Reset cannot cancel in-flight GPU work (regression: this used to discard it)
//   T5 destroying the proxy invalidates a never-submitted recording immediately
//
// Determinism: T2/T4 block the queue with a manual fence wait (queue->Wait) before the
// tracker's Signal, so GetCompletedValue stays 0 until the test releases the block.
#include <thread>
#include <vector>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cassert>
#include <cstdio>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/GpuBatchTracker.h"

using Microsoft::WRL::ComPtr;
namespace S = DlssNr::Submission;
namespace GB = DlssNr::Submission::GpuBatch;

static GB::Tracker::Status StatusOf(GB::Tracker::Ticket t) { return GB::Tracker::Instance().StatusOf(t); }

int main()
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> device;
    assert(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
    assert(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    assert(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))));

    D3D12_COMMAND_QUEUE_DESC qd = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    assert(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))));

    auto makeAllocator = [&]() {
        ComPtr<ID3D12CommandAllocator> alloc;
        assert(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))));
        return alloc;
    };
    auto makeBlockFence = [&]() {
        ComPtr<ID3D12Fence> f;
        assert(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f))));
        return f;
    };
    auto& tracker = GB::Tracker::Instance();
    assert(!tracker.ObserveCompletion(GB::kInvalidTicket));
    assert(!tracker.ObserveCompletion(UINT64_MAX));

    // ---- T1: native list records and retires through the executed fence ----
    {
        ComPtr<ID3D12CommandAllocator> alloc = makeAllocator();
        ComPtr<ID3D12GraphicsCommandList> native;
        assert(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                  IID_PPV_ARGS(&native))));
        const auto ticket = tracker.Record(native.Get());
        assert(ticket != GB::kInvalidTicket);
        const auto receipt = tracker.ObserveCompletion(ticket);
        assert(receipt && receipt->outcome.load() == GB::Tracker::CompletionReceipt::Outcome::Outstanding);
        assert(tracker.ObserveCompletion(ticket) == receipt);
        assert(StatusOf(ticket) == GB::Tracker::Status::Outstanding); // recorded, not yet executed
        assert(SUCCEEDED(native->Close()));
        ID3D12CommandList* t1Lists[] = {native.Get()};
        tracker.OnExecuted(1, t1Lists, queue.Get());
        assert(tracker.Wait(ticket, 5000)); // fence completed -> retired
        assert(StatusOf(ticket) == GB::Tracker::Status::Retired);
        assert(receipt->outcome.load() == GB::Tracker::CompletionReceipt::Outcome::Completed);
    }

    // (Native replays are not counted: a pooled native list re-submits the same pointer
    //  every frame after an invisible Reset, indistinguishable from a true replay.)
    {
        ComPtr<ID3D12CommandAllocator> alloc = makeAllocator();
        ComPtr<ID3D12GraphicsCommandList> native;
        assert(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                  IID_PPV_ARGS(&native))));
        assert(SUCCEEDED(native->Close()));
        const auto ticket = tracker.Record(native.Get());
        const auto receipt = tracker.ObserveCompletion(ticket);
        ID3D12CommandList* lists[] = {native.Get()};
        tracker.OnExecuted(1, lists, queue.Get());
        assert(tracker.Wait(ticket, 5000)); // first execution completes
        assert(receipt->outcome.load() == GB::Tracker::CompletionReceipt::Outcome::Completed);
        const auto replaysBefore = tracker.ReplaysAfterCompletion();

        // Block the queue so the replay's signal cannot complete yet.
        ComPtr<ID3D12Fence> block = makeBlockFence();
        assert(SUCCEEDED(queue->Wait(block.Get(), 1)));
        tracker.OnExecuted(1, lists, queue.Get()); // replay
        assert(StatusOf(ticket) == GB::Tracker::Status::Outstanding); // re-armed, replay in flight
        assert(receipt->outcome.load() == GB::Tracker::CompletionReceipt::Outcome::Outstanding);
        assert(SUCCEEDED(block->Signal(1)));
        assert(tracker.Wait(ticket, 5000));
        assert(StatusOf(ticket) == GB::Tracker::Status::Retired);
        assert(receipt->outcome.load() == GB::Tracker::CompletionReceipt::Outcome::Completed);
        assert(tracker.ReplaysAfterCompletion() == replaysBefore); // native: not counted
    }

    // ---- T2b: a wrapped recording re-submitted at the SAME generation is a true
    // replay and increments the counter (this is the reuse-contract field datum) ----
    {
        ComPtr<ID3D12CommandAllocator> alloc = makeAllocator();
        ComPtr<ID3D12GraphicsCommandList> real;
        assert(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                  IID_PPV_ARGS(&real))));
        ComPtr<S::CommandListProxy> proxy;
        assert(SUCCEEDED(S::CommandListProxy::Create(device.Get(), alloc.Get(), real.Get(), &proxy)));
        const auto ticket = tracker.Record(proxy.Get());
        assert(SUCCEEDED(proxy->Close())); // no Reset: generation stays, replay stays possible
        ID3D12CommandList* lists[] = {proxy.Get()};
        tracker.OnExecuted(1, lists, queue.Get());
        assert(tracker.Wait(ticket, 5000)); // first execution completes
        const auto replaysBefore = tracker.ReplaysAfterCompletion();

        ComPtr<ID3D12Fence> block = makeBlockFence();
        assert(SUCCEEDED(queue->Wait(block.Get(), 1)));
        tracker.OnExecuted(1, lists, queue.Get()); // true replay of the closed recording
        assert(tracker.ReplaysAfterCompletion() == replaysBefore + 1);
        assert(StatusOf(ticket) == GB::Tracker::Status::Outstanding); // re-armed until replay completes
        assert(SUCCEEDED(block->Signal(1)));
        assert(tracker.Wait(ticket, 5000));
    }

    // ---- T3: proxy Reset discards a never-submitted recording at once ----
    {
        ComPtr<ID3D12CommandAllocator> alloc = makeAllocator();
        ComPtr<ID3D12GraphicsCommandList> real;
        assert(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                  IID_PPV_ARGS(&real))));
        ComPtr<S::CommandListProxy> proxy;
        assert(SUCCEEDED(S::CommandListProxy::Create(device.Get(), alloc.Get(), real.Get(), &proxy)));
        const auto ticket = tracker.Record(proxy.Get()); // registers the lifecycle observer
        const auto receipt = tracker.ObserveCompletion(ticket);
        assert(StatusOf(ticket) == GB::Tracker::Status::Outstanding);
        assert(SUCCEEDED(proxy->Close()));
        assert(SUCCEEDED(proxy->Reset(alloc.Get(), nullptr))); // commands erased: never reaches the GPU
        assert(StatusOf(ticket) == GB::Tracker::Status::Retired); // no fence wait involved
        assert(receipt->outcome.load() == GB::Tracker::CompletionReceipt::Outcome::Discarded);
    }

    // ---- T4: proxy Reset does not retire a submitted recording with a pending fence ----
    {
        ComPtr<ID3D12CommandAllocator> alloc = makeAllocator();
        ComPtr<ID3D12GraphicsCommandList> real;
        assert(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                  IID_PPV_ARGS(&real))));
        ComPtr<S::CommandListProxy> proxy;
        assert(SUCCEEDED(S::CommandListProxy::Create(device.Get(), alloc.Get(), real.Get(), &proxy)));
        const auto ticket = tracker.Record(proxy.Get());
        const auto receipt = tracker.ObserveCompletion(ticket);
        assert(SUCCEEDED(proxy->Close()));

        ComPtr<ID3D12Fence> block = makeBlockFence();
        assert(SUCCEEDED(queue->Wait(block.Get(), 1))); // execution (and the tracker's signal) stalls
        ID3D12CommandList* lists[] = {proxy.Get()};
        tracker.OnExecuted(1, lists, queue.Get());
        assert(StatusOf(ticket) == GB::Tracker::Status::Outstanding); // fence pending

        assert(SUCCEEDED(proxy->Reset(alloc.Get(), nullptr))); // generation advances; work already in flight
        assert(StatusOf(ticket) == GB::Tracker::Status::Outstanding); // MUST stay protected until the fence
        assert(receipt->outcome.load() == GB::Tracker::CompletionReceipt::Outcome::Outstanding);

        assert(SUCCEEDED(block->Signal(1)));
        assert(tracker.Wait(ticket, 5000));
        assert(StatusOf(ticket) == GB::Tracker::Status::Retired);
        assert(receipt->outcome.load() == GB::Tracker::CompletionReceipt::Outcome::Completed);
        // Drive reclamation after invalidated GPU work completed. The receipt
        // must retain completion even after its tracker entry has disappeared.
        tracker.OnExecuted(1, lists, queue.Get());
        assert(receipt->outcome.load() == GB::Tracker::CompletionReceipt::Outcome::Completed);
    }

    // ---- T5: destroying the proxy discards a never-submitted recording at once ----
    {
        ComPtr<ID3D12CommandAllocator> alloc = makeAllocator();
        ComPtr<ID3D12GraphicsCommandList> real;
        assert(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                  IID_PPV_ARGS(&real))));
        ComPtr<S::CommandListProxy> proxy;
        assert(SUCCEEDED(S::CommandListProxy::Create(device.Get(), alloc.Get(), real.Get(), &proxy)));
        const auto ticket = tracker.Record(proxy.Get());
        const auto receipt = tracker.ObserveCompletion(ticket);
        assert(SUCCEEDED(proxy->Close()));
        proxy.Reset(); // drop the last reference: proxy destroyed -> Invalidated(current identity)
        assert(StatusOf(ticket) == GB::Tracker::Status::Retired);
        assert(receipt->outcome.load() == GB::Tracker::CompletionReceipt::Outcome::Discarded);
        const auto trigger = tracker.Record(real.Get());
        ID3D12CommandList* lists[] = { real.Get() };
        tracker.OnExecuted(1, lists, queue.Get());
        assert(!tracker.ObserveCompletion(ticket));
        assert(receipt->outcome.load() == GB::Tracker::CompletionReceipt::Outcome::Discarded);
        assert(tracker.Wait(trigger, 5000));
    }

    // ---- T6: a submitted recording is reclaimed only after invalidation AND fence
    // completion; a wrapped pool cycling submit -> Reset every frame must not accumulate ----
    {
        ComPtr<ID3D12CommandAllocator> alloc = makeAllocator();
        ComPtr<ID3D12GraphicsCommandList> real;
        assert(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                                  IID_PPV_ARGS(&real))));
        ComPtr<S::CommandListProxy> proxy;
        assert(SUCCEEDED(S::CommandListProxy::Create(device.Get(), alloc.Get(), real.Get(), &proxy)));
        const size_t before = tracker.TrackedEntries();
        for (int frame = 0; frame < 8; ++frame)
        {
            const auto ticket = tracker.Record(proxy.Get());
            assert(SUCCEEDED(proxy->Close()));
            ID3D12CommandList* lists[] = {proxy.Get()};
            tracker.OnExecuted(1, lists, queue.Get());
            assert(tracker.Wait(ticket, 5000)); // frame submitted and completed
            assert(SUCCEEDED(proxy->Reset(alloc.Get(), nullptr))); // pool recycles the list
        }
        // Reap runs inside OnExecuted's first phase; drive it with one dummy native
        // submission and require the eight invalidated+completed recordings to be gone.
        {
            ComPtr<ID3D12CommandAllocator> dummyAlloc = makeAllocator();
            ComPtr<ID3D12GraphicsCommandList> native;
            assert(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, dummyAlloc.Get(),
                                                      nullptr, IID_PPV_ARGS(&native))));
            const auto dummy = tracker.Record(native.Get());
            ID3D12CommandList* lists[] = {native.Get()};
            tracker.OnExecuted(1, lists, queue.Get());
            (void)dummy;
        }
        assert(tracker.TrackedEntries() <= before + 1); // +1: the dummy native entry
    }

    // ---- T7: concurrent Record/OnExecuted/Reset/StatusOf must not deadlock or race.
    // The P1 fix (identity QI outside mutex_) and the P2b fix (observer_ under mutex_)
    // are exactly this shape; a regression hangs or corrupts here. ----
    {
        constexpr int kThreads = 4, kFrames = 64;
        std::vector<std::thread> workers;
        for (int t = 0; t < kThreads; ++t)
        {
            workers.emplace_back([&, t] {
                ComPtr<ID3D12CommandAllocator> alloc;
                assert(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                IID_PPV_ARGS(&alloc))));
                ComPtr<ID3D12GraphicsCommandList> real;
                assert(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
                                                           nullptr, IID_PPV_ARGS(&real))));
                ComPtr<S::CommandListProxy> proxy;
                assert(SUCCEEDED(S::CommandListProxy::Create(device.Get(), alloc.Get(), real.Get(), &proxy)));
                for (int f = 0; f < kFrames; ++f)
                {
                    const auto ticket = tracker.Record(proxy.Get());
                    proxy->Close();
                    ID3D12CommandList* lists[] = {proxy.Get()};
                    tracker.OnExecuted(1, lists, queue.Get());
                    tracker.StatusOf(ticket); // concurrent poll while others submit
                    if (f % 4 == 3)
                        proxy->Reset(alloc.Get(), nullptr); // concurrent invalidate
                }
            });
        }
        for (auto& w : workers)
            w.join();
        // Completing within the harness timeout is the assertion: ABBA would hang here.
    }

    std::printf("gpu_batch_tracker: PASS\n");
    return 0;
}
