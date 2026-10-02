#include "lmxxf_gpu_test_utils.h"
#include <wrl/client.h>
#include <memory>
#include <vector>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfRecordingOwner.h"

template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
struct RecordedFrame
{
    void* token = nullptr;
    ID3D12Resource* output = nullptr; // owned by runtime lease
    Ptr<ID3D12CommandAllocator> producerAllocator, consumerAllocator;
    Ptr<ID3D12GraphicsCommandList> producer, consumer;
    Ptr<ID3D12Fence> fence;
    UINT64 value = 0;
};
int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    Ptr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    HMODULE dll = LoadLibraryW(Widen(argv[1]).c_str()); Require(dll != nullptr, "runtime load");
    auto getApi = reinterpret_cast<int32_t(*)(uint32_t, LmxxfNrApi*)>(GetProcAddress(dll, "LmxxfNrGetApi"));
    Require(getApi != nullptr, "runtime export"); LmxxfNrApi api {}; api.struct_size = sizeof api;
    Require(getApi(2, &api) == LMXXF_NR_OK && api.CollectRecording, "v2 table");
    auto getTiming = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrTimingApi*)>(GetProcAddress(dll, "LmxxfNrGetTimingApi"));
    LmxxfNrTimingApi timing {}; timing.struct_size = sizeof timing;
    Require(getTiming && getTiming(NR_TIMING_VERSION, &timing) == LMXXF_NR_OK, "GPU timing extension");
    auto ok = [&](int32_t result, const char* step) {
        if (result != LMXXF_NR_OK) { char error[256] {}; api.GetLastError(error, sizeof error); std::fprintf(stderr, "%s rc=%d: %s\n", step, result, error); }
        Require(result == LMXXF_NR_OK, step);
    };
    Ptr<IDXGIFactory4> factory; Ptr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory"); SIZE_T best = 0;
    for (UINT i = 0;; ++i) {
        Ptr<IDXGIAdapter1> adapter; if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc {}; adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x1002 && desc.DedicatedVideoMemory > best) {
            Ptr<ID3D12Device> candidate;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&candidate)))) {
                device = candidate; best = desc.DedicatedVideoMemory;
            }
        }
    }
    Require(device != nullptr, "AMD device");
    Ptr<ID3D12CommandQueue> queue, other; D3D12_COMMAND_QUEUE_DESC qd {};
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&other)), "other queue");
    const auto modules = Widen(argv[2]);
    auto create = [&] {
        LmxxfNrCreateInfo info {}; info.struct_size = sizeof info; info.device = device.Get(); info.queue = queue.Get();
        info.assets_directory = modules.c_str(); info.flags = LMXXF_NR_CREATE_FLAG_RECORDING_LEASES;
        void* context = nullptr; ok(api.Create(&info, &context), "create lease session");
        ok(api.PrepareSession(context), "prepare session"); return context;
    };
    void* context = create();
    Require(timing.SetEnabled(context, 1) == LMXXF_NR_OK, "enable GPU telemetry");
    auto makeColour = [&](UINT width, UINT height) {
        D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width; rd.Height = height; rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Ptr<ID3D12Resource> colour;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             nullptr, IID_PPV_ARGS(&colour)), "colour");
        UploadColorPattern(device.Get(), queue.Get(), colour.Get());
        return colour;
    };
    uint64_t nextFrame = 0;
    auto record = [&](void* owner, UINT width, UINT height, bool passthrough = false, bool exposure = false) {
        auto result = std::make_unique<RecordedFrame>();
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&result->producerAllocator)), "producer allocator");
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&result->consumerAllocator)), "consumer allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, result->producerAllocator.Get(), nullptr,
                                       IID_PPV_ARGS(&result->producer)), "producer list");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, result->consumerAllocator.Get(), nullptr,
                                       IID_PPV_ARGS(&result->consumer)), "consumer list");
        Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&result->fence)), "tail fence");
        auto colour = makeColour(width, height);
        LmxxfNrFrameInfo frame {}; frame.struct_size = sizeof frame;
        frame.frame_id = ++nextFrame;
        frame.command_list = result->producer.Get(); frame.color = colour.Get(); frame.color_width = width; frame.color_height = height;
        frame.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        frame.paper_white = frame.pre_exposure = frame.exposure_scale = frame.model_scale = 1;
        frame.flags = (passthrough ? LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH : 0) | (exposure ? LMXXF_NR_FRAME_FLAG_AUTO_EXPOSURE : 0);
        LmxxfNrJob job {}; job.struct_size = sizeof job;
        ok(api.PrepareFrame(owner, &frame, &job), "prepare recording"); result->token = job.handle;
        result->output = static_cast<ID3D12Resource*>(job.private_output);
        ok(api.RecordInputs(owner, job.handle, result->producer.Get()), "record inputs");
        Check(result->producer->Close(), "producer close");
        ok(api.RecordOutputs(owner, job.handle, result->consumer.Get()), "record outputs");
        Check(result->consumer->Close(), "consumer close");
        // Runtime must retain the exact colour allocation after this return.
        return result;
    };
    auto discard = [&](void* owner, std::unique_ptr<RecordedFrame>& frame) {
        const auto token = frame->token;
        frame->producer.Reset(); frame->consumer.Reset(); // recording invalidated before releasing pins
        ok(api.InvalidateRecording(owner, token), "invalidate recording");
        ok(api.CollectRecording(owner, token), "collect completed/unsubmitted recording");
        Require(api.CollectRecording(owner, token) == LMXXF_NR_INVALID_ARGUMENT, "late token rejected"); frame.reset();
    };
    auto abandoned = record(context, 1280, 720); const auto oldToken = abandoned->token; discard(context, abandoned);
    std::vector<std::unique_ptr<RecordedFrame>> frames;
    for (unsigned i = 0; i < 10; ++i) {
        frames.push_back(record(context, 1280, 720));
        Require(frames.back()->token != oldToken, "discarded token never aliases a new Job");
        Require(api.CollectRecording(context, frames.back()->token) == LMXXF_NR_UNAVAILABLE, "live delayed recording retained");
    }
    Require(api.Destroy(context) == LMXXF_NR_UNAVAILABLE, "Destroy refuses live recordings");
    auto run = [&](void* owner, RecordedFrame& frame, ID3D12CommandQueue* target) {
        ok(api.BeginRecordingExecution(owner, frame.token, target), "begin execution");
        ID3D12CommandList* producer[] = {frame.producer.Get()}; target->ExecuteCommandLists(1, producer);
        ok(api.EnqueueHip(owner, frame.token, target), "actual HIP enqueue");
        ID3D12CommandList* consumer[] = {frame.consumer.Get()}; target->ExecuteCommandLists(1, consumer);
        const HRESULT signal = target->Signal(frame.fence.Get(), ++frame.value);
        ok(api.EndRecordingExecution(owner, frame.token, target, 3, frame.fence.Get(), frame.value, signal), "end execution");
        WaitQueue(device.Get(), target);
        if (owner == context) { LmxxfNrTimings net {}; net.struct_size = sizeof net; ok(api.GetTimings(owner, &net), "completed network timing"); }
        return HashTexture(device.Get(), target, frame.output);
    };
    LmxxfNrTimings net {}; net.struct_size = sizeof net;
    ok(api.GetTimings(context, &net), "lazy network timing request");
    Require(!net.valid, "first request cannot fabricate a sample");
    char netStatus[1024] {}; ok(api.GetStatus(context, netStatus, sizeof netStatus), "timing availability");
    const bool blockedPdl = std::strstr(netStatus, "unavailable-pdl") != nullptr;
    const auto baseline = run(context, *frames.front(), other.Get());
    run(context, *frames.back(), queue.Get());
    Require(run(context, *frames.front(), other.Get()) == baseline, "old immutable binding survives ten rebinds and replay");
    auto passthrough = record(context, 1280, 720, true);
    run(context, *passthrough, queue.Get());
    Require(run(context, *frames.front(), other.Get()) == baseline, "HIP replay after passthrough shared-resource use");
    auto metered = record(context, 1280, 720, false, true); run(context, *metered, queue.Get());
    auto resized = record(context, 1920, 1080); run(context, *resized, other.Get());
    for (unsigned i = 0; i < 8; ++i) {
        auto next = record(context, 1920, 1080); run(context, *next, other.Get());
        ok(api.GetTimings(context, &net), "rebuilt network timing");
        Require(blockedPdl ? !net.valid : net.valid && net.frame_id == nextFrame && net.network_ms > 0, "PDL is explicit unavailable; supported network reports current frame");
        std::printf("runtime network frame=%llu ms=%.3f\n", net.frame_id, net.network_ms);
        discard(context, next);
    }
    Require(run(context, *frames.front(), queue.Get()) == baseline, "old bridge/codec recording survives geometry replacement");
    void* newContext = create(); auto fresh = record(newContext, 1280, 720); run(newContext, *fresh, other.Get());
    Require(run(context, *frames.front(), queue.Get()) == baseline, "old recording survives another session");
    discard(newContext, fresh); ok(api.Destroy(newContext), "destroy new session asynchronously");
    discard(context, resized); discard(context, metered); discard(context, passthrough);
    for (auto& frame : frames) discard(context, frame);
    NrTimingSnapshot measured {}; measured.struct_size = sizeof measured;
    ok(timing.GetSnapshot(context, &measured), "read completed GPU timing");
    Require(blockedPdl ? !measured.stages[NR_GPU_NETWORK].samples && (measured.reserved & 2u) : measured.stages[NR_GPU_NETWORK].samples > 0 && measured.stages[NR_GPU_NETWORK].last_ms > 0,
            "runtime exposes actual HIP durations");
    std::printf("NR GPU samples=%llu median_ms=%.3f last_ms=%.3f\n",
                measured.stages[NR_GPU_NETWORK].samples, measured.stages[NR_GPU_NETWORK].mean_ms,
                measured.stages[NR_GPU_NETWORK].last_ms);
    Require(measured.stages[NR_GPU_ENCODE].samples > 0 && measured.stages[NR_GPU_ENCODE].last_ms > 0 &&
            measured.stages[NR_GPU_DECODE].samples > 0 && measured.stages[NR_GPU_DECODE].last_ms > 0,
            "runtime exposes completed D3D encode and decode durations");
    ok(api.Destroy(context), "destroy old session asynchronously");
    // Exercise the actual host owner + proxy contract, without a backend instance
    // that could keep the old context alive accidentally after NR off/switch.
    namespace Ownership = DlssNr::Backend::LmxxfRecording;
    namespace Submission = DlssNr::Submission;
    struct ProxyFrame {
        Ptr<ID3D12CommandAllocator> allocator;
        Ptr<ID3D12GraphicsCommandList> native;
        Submission::CommandListProxy* proxy = nullptr;
        ID3D12Resource* output = nullptr;
    };
    auto recordProxy = [&](const std::shared_ptr<Ownership::SessionOwner>& owner, bool partial = false) {
        auto result = std::make_unique<ProxyFrame>();
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&result->allocator)), "host allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, result->allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&result->native)), "host native list");
        Check(Submission::CommandListProxy::Create(device.Get(), result->allocator.Get(), result->native.Get(),
                                                  &result->proxy), "host proxy");
        auto colour = makeColour(1280, 720);
        LmxxfNrFrameInfo frame {}; frame.struct_size = sizeof frame;
        frame.color = colour.Get(); frame.command_list = result->proxy; frame.color_width = 1280; frame.color_height = 720;
        frame.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        frame.paper_white = frame.pre_exposure = frame.exposure_scale = frame.model_scale = 1;
        LmxxfNrJob job {}; job.struct_size = sizeof job;
        ok(api.PrepareFrame(owner->context, &frame, &job), "host prepare");
        std::lock_guard lifetime(Submission::RecordingMutex());
        auto lease = Ownership::Attach(owner, job.handle, result->proxy); Require(lease != nullptr, "host attach observer");
        ok(api.RecordInputs(owner->context, job.handle, result->proxy), "host inputs");
        if (partial)
        {
            Require(api.RecordOutputs(owner->context, job.handle, nullptr) == LMXXF_NR_INVALID_ARGUMENT,
                    "failed output recording after private input commands");
            owner->failed = true; // Same host response as FinishRecord failure.
        }
        else
        {
            Check(result->proxy->SplitSegments(), "host split");
            ok(api.RecordOutputs(owner->context, job.handle, result->proxy), "host outputs");
            lease->ready = true;
        }
        Check(result->proxy->Close(), "host close");
        result->output = static_cast<ID3D12Resource*>(job.private_output);
        return result;
    };
    auto activeOwner = Ownership::SessionOwner::Create(api, create()); Require(activeOwner != nullptr, "host owner pin");
    std::weak_ptr<Ownership::SessionOwner> oldOwner = activeOwner;
    auto oldProxy = recordProxy(activeOwner);
    activeOwner.reset(); // NR off: only the recording retains the session.
    Require(!oldOwner.expired(), "NR off retains a live unsubmitted recording");
    Check(oldProxy->proxy->ExecuteOn(other.Get()), "old recording after NR off"); WaitQueue(device.Get(), other.Get());
    const auto hostBaseline = HashTexture(device.Get(), other.Get(), oldProxy->output);
    Require(hostBaseline == baseline, "host observer runs HIP for old recording");
    activeOwner = Ownership::SessionOwner::Create(api, create()); auto newProxy = recordProxy(activeOwner);
    Check(newProxy->proxy->ExecuteOn(queue.Get()), "new active session"); WaitQueue(device.Get(), queue.Get());
    Check(oldProxy->proxy->ExecuteOn(other.Get()), "old recording replay after new session"); WaitQueue(device.Get(), other.Get());
    Require(HashTexture(device.Get(), other.Get(), oldProxy->output) == hostBaseline, "off/on preserves old recorded output");
    newProxy->proxy->Release(); newProxy.reset(); activeOwner.reset();
    // Hold the actual queue: Reset invalidates the recording but must retain the
    // session while its producer/HIP/consumer are pending, even without future Record calls.
    Ptr<ID3D12Fence> gate; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "retirement gate");
    Check(other->Wait(gate.Get(), 1), "block old queue");
    Check(oldProxy->proxy->ExecuteOn(other.Get()), "pending old execution");
    Ptr<ID3D12CommandAllocator> replacement;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&replacement)), "replacement allocator");
    Check(oldProxy->proxy->Reset(replacement.Get(), nullptr), "Reset while last execution pending");
    Require(!oldOwner.expired(), "Reset cannot release GPU-live lease");
    Check(gate->Signal(1), "unblock old queue"); WaitQueue(device.Get(), other.Get());
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (!oldOwner.expired() && GetTickCount64() < deadline) Sleep(10);
    Require(oldOwner.expired(), "background collector releases invalidated owner without active backend");
    Check(oldProxy->proxy->Close(), "close replacement generation"); oldProxy->proxy->Release(); oldProxy.reset();
    // A failed recording still contains input copies on the game list. It must
    // execute and retain its private resources even though HIP was never armed.
    activeOwner = Ownership::SessionOwner::Create(api, create());
    std::weak_ptr<Ownership::SessionOwner> partialOwner = activeOwner;
    auto partial = recordProxy(activeOwner, true);
    activeOwner.reset();
    const auto enqueues = DlssNr::Backend::LmxxfCut::Pending().enqueueCalls.load();
    Check(partial->proxy->ExecuteOn(queue.Get()), "execute partial recording");
    WaitQueue(device.Get(), queue.Get());
    Check(partial->proxy->ExecuteOn(other.Get()), "replay partial recording");
    WaitQueue(device.Get(), other.Get());
    Require(DlssNr::Backend::LmxxfCut::Pending().enqueueCalls.load() == enqueues,
            "partial recording never enqueues HIP");
    partial->proxy->Release(); partial.reset();
    Require(partialOwner.expired(), "partial recording collected after final Release");

    // Invalid queue facts cannot certify the begun execution. A failed Signal
    // must stay unconfirmed even if a later unrelated fence completes.
    void* faultContext = create(); auto fault = record(faultContext, 1280, 720, true);
    ok(api.BeginRecordingExecution(faultContext, fault->token, queue.Get()), "fault begin");
    Require(api.EndRecordingExecution(faultContext, fault->token, other.Get(), 0, nullptr, 0, S_OK)
            == LMXXF_NR_INVALID_ARGUMENT, "wrong actual queue rejected");
    ID3D12CommandList* faultLists[] = {fault->producer.Get(), fault->consumer.Get()};
    queue->ExecuteCommandLists(2, faultLists);
    Require(api.EndRecordingExecution(faultContext, fault->token, queue.Get(), 3, nullptr, 0, E_FAIL)
            == LMXXF_NR_UNAVAILABLE, "failed signal leaves no certificate");
    WaitQueue(device.Get(), queue.Get());
    Require(api.BeginRecordingExecution(faultContext, fault->token, queue.Get()) == LMXXF_NR_UNAVAILABLE,
            "uncertified chain cannot replay");
    fault->producer.Reset(); fault->consumer.Reset();
    ok(api.InvalidateRecording(faultContext, fault->token), "invalidate uncertified work");
    Require(api.CollectRecording(faultContext, fault->token) == LMXXF_NR_UNAVAILABLE,
            "unrelated completion cannot retire failed Signal");
    Require(api.Destroy(faultContext) == LMXXF_NR_UNAVAILABLE, "uncertified session retained until process exit");
    // Deliberately retained faultContext models the production fail-closed path.
    Ptr<ID3D12InfoQueue> info;
    if (SUCCEEDED(device.As(&info)))
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0; info->GetMessage(i, nullptr, &size); std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(info->GetMessage(i, message, &size), "debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) std::fprintf(stderr, "%s\n", message->pDescription);
            Require(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, "runtime D3D12 debug errors");
        }
    // The DLL must pin its own destructor callbacks when the host drops its module reference.
    FreeLibrary(dll);
    std::printf("runtime recording leases: PASS baseline=%016llx\n", static_cast<unsigned long long>(baseline));
}
