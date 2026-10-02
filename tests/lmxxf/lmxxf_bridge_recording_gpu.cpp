#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "third_party/lmxxf/Development/HIP/hip_reference_network.h"
#include "third_party/lmxxf/src/native_device_identity.h"
#include "third_party/lmxxf/Development/HIP/hip_d3d12_bridge.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <cmath>

namespace hip_reference {
struct BridgeTimingTest {
    static auto& Query(D3D12Bridge& bridge) { return bridge.timing_query; }
    static bool NewEventsAllocated(const D3D12Bridge& bridge) { return bridge.timing_begin[0] != nullptr; }
    static auto& Elapsed(D3D12Bridge& bridge) { return bridge.network->Runtime().hipEventElapsedTime; }
    static unsigned Quarantined(const D3D12Bridge& bridge) {
        return unsigned(std::count_if(bridge.timing_slots.begin(), bridge.timing_slots.end(),
            [](const auto& slot) { return slot.pending && slot.completion == UINT64_MAX; }));
    }
};
}

static void Require(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

static void Check(HRESULT hr, const char *what)
{
    if (FAILED(hr))
    {
        std::fprintf(stderr, "FAIL: %s hr=%08lx\n", what, static_cast<unsigned long>(hr));
        std::exit(1);
    }
}

static void WaitQueue(ID3D12Device *device, ID3D12CommandQueue *queue)
{
    ID3D12Fence *fence = nullptr;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    Check(queue->Signal(fence, 1), "signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "completion event");
    Check(fence->SetEventOnCompletion(1, event), "event on completion");
    Require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0 && fence->GetCompletedValue() >= 1,
            "queue completion");
    CloseHandle(event);
    fence->Release();
}

static ID3D12Resource *Buffer(ID3D12Device *device, D3D12_HEAP_TYPE type, UINT64 bytes,
                              D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *resource = nullptr;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&resource)), "buffer");
    return resource;
}

static void Transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
                       D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}


int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    ID3D12Debug* debug = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); debug->Release(); }
    IDXGIFactory4* factory = nullptr; ID3D12Device* device = nullptr;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    for (UINT i = 0; !device; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc {}; adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x1002) D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        adapter->Release();
    }
    factory->Release(); Require(device != nullptr, "AMD device");
    ID3D12CommandQueue *creation = nullptr, *actual = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd {};
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&creation)), "creation queue");
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&actual)), "actual queue");
    constexpr UINT width = 512, height = 512;
    constexpr UINT64 pixels = UINT64(width) * height, bytes = pixels * 12;
    hip_reference::Options options; options.width = width; options.height = height;
    options.assets = argv[1]; options.modules = argv[2]; options.wmma = options.wave = options.pooled = true;
    options.fast_prefix = true;
    try {
        hip_reference::D3D12Bridge bridge; bridge.Create(creation, options, {});
        bridge.PrepareStagedKernels(); bridge.EnableRecordingLeases();
        auto* input = Buffer(device, D3D12_HEAP_TYPE_DEFAULT, pixels * 16, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto* readback = Buffer(device, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12CommandAllocator *pa = nullptr, *ca = nullptr;
        ID3D12GraphicsCommandList *producer = nullptr, *consumer = nullptr;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pa)), "producer allocator");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)), "consumer allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pa, nullptr, IID_PPV_ARGS(&producer)), "producer");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&consumer)), "consumer");
        // Initialize a nonzero RGB input, independently of the recording we discard.
        auto* upload = Buffer(device, D3D12_HEAP_TYPE_UPLOAD, pixels * 16, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr; Check(upload->Map(0, nullptr, &mapped), "input upload");
        auto* rgba = static_cast<float*>(mapped);
        for (UINT64 i = 0; i < pixels; ++i) {
            rgba[i*4] = float(i%31)/32.f; rgba[i*4+1] = .5f; rgba[i*4+2] = .75f; rgba[i*4+3] = 1.f;
        }
        upload->Unmap(0, nullptr);
        Transition(producer, input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        producer->CopyBufferRegion(input, 0, upload, 0, pixels * 16);
        Transition(producer, input, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Check(producer->Close(), "input upload close");
        ID3D12CommandList* setup[] = {producer}; actual->ExecuteCommandLists(1, setup); WaitQueue(device, actual);
        upload->Release(); Check(pa->Reset(), "input upload reset"); Check(producer->Reset(pa, nullptr), "input upload list reset");
        // First recording is discarded without ever being executed.
        bridge.RecordInputCopy(producer, input); bridge.CancelUnsubmitted();
        Check(producer->Close(), "discard close"); Check(pa->Reset(), "discard allocator reset");
        Check(producer->Reset(pa, nullptr), "discard reset");
        bridge.RecordInputCopy(producer, input); Check(producer->Close(), "producer close");
        bridge.RecordOutputReadable(consumer);
        Transition(consumer, bridge.Output(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        consumer->CopyBufferRegion(readback, 0, bridge.Output(), 0, bytes);
        Transition(consumer, bridge.Output(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        bridge.SealRecordedOutput(consumer); Check(consumer->Close(), "consumer close");
        Require(bridge.CurrentPhase() == hip_reference::D3D12Bridge::Phase::Ready, "sealed recording is not an execution");
        // A refused/unsubmitted execution leaves the same lease replayable.
        bridge.BeginRecordedExecution(actual); bridge.EndRecordedExecution(actual, false, false);
        std::vector<unsigned char> baseline;
        hip_reference::D3D12Bridge::ProductionTiming timing;
        Require(!bridge.TakeProductionTiming(timing), "no GPU time before execution");
        for (unsigned pass = 0; pass < 11; ++pass) {
            ID3D12CommandQueue* q = pass % 2 ? creation : actual;
            bridge.BeginRecordedExecution(q);
            bridge.ConfigureProductionTiming(pass != 0, 42, pass, 7);
            ID3D12CommandList* first[] = {producer}; q->ExecuteCommandLists(1, first);
            bridge.EnqueueAfterProducer(q, 1);
            // Discard one actual execution after HIP, without submitting consumer.
            if (pass == 1) { bridge.EndRecordedExecution(q, true, false); continue; }
            ID3D12CommandList* second[] = {consumer}; q->ExecuteCommandLists(1, second);
            bridge.EndRecordedExecution(q, true, true); WaitQueue(device, q);
            Require(SUCCEEDED(device->GetDeviceRemovedReason()), "device remained live");
            void* data = nullptr; D3D12_RANGE range {0, SIZE_T(bytes)};
            Check(readback->Map(0, &range, &data), "readback");
            const auto* values = static_cast<const float*>(data); bool finite = true, nonzero = false;
            for (UINT64 i = 0; i < pixels * 3; ++i) { finite = finite && std::isfinite(values[i]); nonzero = nonzero || std::fabs(values[i]) > 1e-8f; }
            Require(finite && nonzero, "finite nonzero neural output");
            if (baseline.empty()) baseline.assign(static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + bytes);
            else Require(std::memcmp(baseline.data(), data, SIZE_T(bytes)) == 0, "replayed neural output bytes");
            readback->Unmap(0, nullptr);
        }
        Require(bridge.WaitForSubmittedWork(), "drain actual last execution queue");
        unsigned samples = 0;
        while (bridge.TakeProductionTiming(timing)) {
            Require(std::isfinite(timing.ms) && timing.ms > 0, "completed finite HIP duration");
            Require(timing.frame == 42 && timing.execution > 0 && timing.execution <= 8 && timing.epoch == 7,
                    "timing execution identity");
            ++samples;
        }
        Require(samples == 8 && bridge.TakeProductionTimingDrops() == 2, "full event pool drops samples without reusing in-flight storage");
        // ABBA wall-clock measurement includes queue submission and completion, independent
        // of the HIP duration under test. It is diagnostic, not a noisy performance gate.
        for (unsigned batch = 0; batch < 4; ++batch) {
            const bool enabled = batch == 1 || batch == 2;
            double wallMs = 0, enqueueMs = 0;
            constexpr unsigned iterations = 40;
            unsigned collected = 0;
            auto collect = [&] {
                while (bridge.TakeProductionTiming(timing)) {
                    Require(enabled, "disabled timing does not produce samples");
                    Require(timing.epoch == 8 && timing.frame == 43 + batch && timing.execution == collected + 1,
                            "reused timing identity and collection order");
                    ++collected;
                }
            };
            for (unsigned i = 0; i < iterations; ++i) {
                const auto start = std::chrono::steady_clock::now();
                bridge.BeginRecordedExecution(actual);
                bridge.ConfigureProductionTiming(enabled, 43 + batch, i + 1, 8);
                ID3D12CommandList* first[] = {producer}; actual->ExecuteCommandLists(1, first);
                const auto enqueueStart = std::chrono::steady_clock::now();
                bridge.EnqueueAfterProducer(actual, 1);
                const auto enqueueEnd = std::chrono::steady_clock::now();
                ID3D12CommandList* second[] = {consumer}; actual->ExecuteCommandLists(1, second);
                bridge.EndRecordedExecution(actual, true, true);
                WaitQueue(device, actual);
                const auto end = std::chrono::steady_clock::now();
                if (i >= 8) {
                    wallMs += std::chrono::duration<double, std::milli>(end - start).count();
                    enqueueMs += std::chrono::duration<double, std::milli>(enqueueEnd - enqueueStart).count();
                }
                collect();
            }
            // The driver may publish event bookkeeping after the external queue completes.
            // Only this harness waits for statistics; production returns to rendering.
            const auto deadline = GetTickCount64() + 1000;
            while (enabled && collected != iterations && GetTickCount64() < deadline) { Sleep(1); collect(); }
            Require(collected == (enabled ? iterations : 0) && bridge.TakeProductionTimingDrops() == 0,
                    "all ABBA samples eventually readable without event reuse races");
            std::printf("timing ABBA batch=%u enabled=%u wall_ms=%.4f enqueue_cpu_ms=%.4f n=%u\n",
                        batch, unsigned(enabled), wallMs / (iterations - 8), enqueueMs / (iterations - 8), iterations - 8);
        }
        // Official fe4d1d73 event-query path: lazy enable, frame tags, disable/re-enable,
        // epoch rejection and failure quarantine. Compare actual output bytes as well.
        Require(!bridge.NetworkTimingEnabled() && !hip_reference::BridgeTimingTest::NewEventsAllocated(bridge),
                "official timing has no events before first request");
        bridge.ConfigureProductionTiming(false, 0, 0, 0);
        bridge.SetTimingEpoch(10);
        Require(bridge.EnableNetworkTiming() && !bridge.PollNetworkTiming().valid, "first request has no sample");
        auto executeTimed = [&](unsigned tag) {
            bridge.SetTimingTag(tag); bridge.BeginRecordedExecution(actual);
            ID3D12CommandList* first[] = {producer}; actual->ExecuteCommandLists(1, first);
            bridge.EnqueueAfterProducer(actual, 1);
            ID3D12CommandList* second[] = {consumer}; actual->ExecuteCommandLists(1, second);
            bridge.EndRecordedExecution(actual, true, true); WaitQueue(device, actual);
        };
        for (unsigned frame = 1; frame <= 8; ++frame) {
            executeTimed(frame);
            auto sample = bridge.PollNetworkTiming();
            const auto deadline = GetTickCount64() + 1000;
            while ((!sample.valid || sample.tag != frame) && GetTickCount64() < deadline) {
                Sleep(1); sample = bridge.PollNetworkTiming(); // harness-only wait
            }
            Require(sample.valid && sample.tag == frame && std::isfinite(sample.ms) && sample.ms > 0,
                    "official completed network span and tag");
            std::printf("official timing frame=%u net_gpu_ms=%.3f\n", frame, sample.ms);
        }
        executeTimed(9); // leave a completed but unread old-epoch event
        bridge.PauseNetworkTiming(); bridge.SetTimingEpoch(11);
        Require(!bridge.PollNetworkTiming().valid, "disabled samples hidden");
        Require(bridge.EnableNetworkTiming() && !bridge.PollNetworkTiming().valid, "old epoch cannot reappear");
        auto& query = hip_reference::BridgeTimingTest::Query(bridge);
        const auto realQuery = query; query = [](hip_probe::Handle) -> int { return 999; };
        executeTimed(10); // fail the immediate non-blocking end-event query
        Require(!bridge.PollNetworkTiming().valid && !bridge.EnableNetworkTiming(), "end-event query error disables timing permanently for bridge");
        query = realQuery;
        executeTimed(11); // rendering survives the latched instrumentation failure
        void* timedData = nullptr; D3D12_RANGE timedRange {0, SIZE_T(bytes)};
        Check(readback->Map(0, &timedRange, &timedData), "official timing readback");
        Require(std::memcmp(baseline.data(), timedData, SIZE_T(bytes)) == 0, "official timing preserves output bytes");
        readback->Unmap(0, nullptr);
        // Real GPU submissions with a failing telemetry API must continue to
        // render, permanently quarantining the failed event pairs until teardown.
        auto& elapsed = hip_reference::BridgeTimingTest::Elapsed(bridge);
        const auto realElapsed = elapsed;
        elapsed = [](float*, hip_probe::Handle, hip_probe::Handle) -> int { return 999; };
        for (unsigned i = 0; i < 9; ++i) {
            bridge.BeginRecordedExecution(actual);
            bridge.ConfigureProductionTiming(true, 99, i + 1, 9);
            ID3D12CommandList* first[] = {producer}; actual->ExecuteCommandLists(1, first);
            bridge.EnqueueAfterProducer(actual, 1);
            ID3D12CommandList* second[] = {consumer}; actual->ExecuteCommandLists(1, second);
            bridge.EndRecordedExecution(actual, true, true); WaitQueue(device, actual);
            Require(!bridge.TakeProductionTiming(timing), "failed telemetry never publishes a sample");
            Require(hip_reference::BridgeTimingTest::Quarantined(bridge) == (std::min)(i + 1, 8u),
                    "failed HIP events never return to reusable pool");
        }
        elapsed = realElapsed;
        void* finalData = nullptr; D3D12_RANGE finalRange {0, SIZE_T(bytes)};
        Check(readback->Map(0, &finalRange, &finalData), "failed telemetry readback");
        Require(std::memcmp(baseline.data(), finalData, SIZE_T(bytes)) == 0, "telemetry failures preserve neural output bytes");
        readback->Unmap(0, nullptr);
        Require(bridge.TakeProductionTimingDrops() == 9, "errors and exhausted event pool only drop statistics");
        producer->Release(); consumer->Release(); pa->Release(); ca->Release(); input->Release(); readback->Release();
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    ID3D12InfoQueue* info = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&info)))) {
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0; info->GetMessage(i, nullptr, &size); std::vector<char> storage(size);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(storage.data()); Check(info->GetMessage(i, m, &size), "debug message");
            if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) std::fprintf(stderr, "%s\n", m->pDescription);
            Require(m->Severity > D3D12_MESSAGE_SEVERITY_ERROR, "debug layer errors");
        }
        info->Release();
    }
    actual->Release(); creation->Release(); device->Release();
    std::puts("bridge recording leases: PASS (discard, real HIP replay, actual queue, byte readback)");
}
