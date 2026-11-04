// Real production shader dispatches, proxy lifecycle, and readback-visible results.
#include <pch.h>
#include "../lmxxf/lmxxf_gpu_test_utils.h"
#include "../lmxxf/queue_faults.h"
#include <shaders/bias/Bias_Dx12.h>
#include <shaders/output_scaling/OS_Dx12.h>
#include <d3d12sdklayers.h>
#include <array>
#include <vector>
#include <memory>
#include <cmath>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/Shader_Common.cpp"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/Shader_Dx12.cpp"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/gpu_time/GpuTime_Dx12.cpp"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/misc/DeferredRelease.cpp"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/bias/Bias_Dx12.cpp"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/output_scaling/OS_Dx12.cpp"

Config::Config() = default;
Config* Config::Instance() { static Config config; return &config; }
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
namespace Submission = DlssNr::Submission;
namespace Util
{
void GetDeviceRemovedReason(ID3D12Device* pDevice)
{
    if (!pDevice) return;
    const auto reason = pDevice->GetDeviceRemovedReason();
    std::fprintf(stderr, "D3D12 device removed reason: 0x%08lx\n", static_cast<unsigned long>(reason));
}
}

struct BiasProbe : Bias_Dx12
{
    using Bias_Dx12::Bias_Dx12;
    std::weak_ptr<RecordedShader::Dispatch> LastDispatch() { return _dispatchSlots.back(); }
    std::weak_ptr<RecordedShader::Intermediate> LastBuffer() { return _recordedBuffer; }
    size_t SlotCount() const { return _dispatchSlots.size(); }
};
struct Recording
{
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<Submission::CommandListProxy> list;
    Ptr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT width = 0, height = 0;
    std::weak_ptr<RecordedShader::Dispatch> dispatch;
    std::weak_ptr<RecordedShader::Intermediate> intermediate;
};
static Recording NewRecording(ID3D12Device* device)
{
    Recording r;
    Ptr<ID3D12GraphicsCommandList> native;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&r.allocator)), "allocator");
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r.allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&native)), "native list");
    Submission::CommandListProxy* proxy = nullptr;
    Check(Submission::CommandListProxy::Create(device, r.allocator.Get(), native.Get(), &proxy), "proxy");
    r.list.Attach(proxy);
    return r;
}
static Ptr<ID3D12Resource> Source(ID3D12Device* device, ID3D12CommandQueue* queue, UINT width, UINT height)
{
    auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT, width, height, 1, 1);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    Ptr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&resource)), "source");
    UploadColorPattern(device, queue, resource.Get());
    return resource;
}
static void Capture(ID3D12Device* device, Recording& r, ID3D12Resource* resource)
{
    auto desc = resource->GetDesc();
    r.width = UINT(desc.Width); r.height = desc.Height;
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &r.footprint, nullptr, nullptr, &bytes);
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r.readback)), "capture buffer");
    D3D12_TEXTURE_COPY_LOCATION source {}, dest {};
    source.pResource = resource; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dest.pResource = r.readback.Get(); dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dest.PlacedFootprint = r.footprint;
    r.list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
}
static Recording RecordBias(ID3D12Device* device, BiasProbe& shader, ID3D12Resource* source, float bias)
{
    auto r = NewRecording(device);
    Require(shader.CreateBufferResource(device, source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, r.list.Get()), "owned bias intermediate");
    auto cleanup = shader.RecordedBufferCleanup(r.list.Get());
    Require(bool(cleanup), "exact intermediate cleanup");
    shader.SetBufferState(r.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Require(shader.Dispatch(r.list.Get(), source, bias, shader.Buffer()), "bias dispatch");
    r.dispatch = shader.LastDispatch(); r.intermediate = shader.LastBuffer();
    shader.SetBufferState(r.list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    Capture(device, r, shader.Buffer());
    cleanup();
    Check(r.list->Close(), "close bias");
    return r;
}
static void Pixels(const Recording& r, float bias)
{
    void* mapped = nullptr;
    Check(r.readback->Map(0, nullptr, &mapped), "readback map");
    std::vector<unsigned char> row(size_t(r.width) * sizeof(float) * 4);
    for (UINT y = 0; y < r.height; ++y) {
        FillRow(row.data(), y, r.width, DXGI_FORMAT_R32G32B32A32_FLOAT);
        const auto* expected = reinterpret_cast<const float*>(row.data());
        const auto* actual = reinterpret_cast<const float*>(static_cast<const unsigned char*>(mapped) +
                                                           size_t(y) * r.footprint.Footprint.RowPitch);
        for (UINT x = 0; x < r.width; ++x)
            for (UINT c = 0; c < 3; ++c)
                Require(std::fabs(actual[x * 4 + c] - expected[x * 4 + c] * (c == 0 ? bias : 1.f)) < 0.00001f,
                        "consumer pixels retain exact dispatch constants and descriptors");
    }
    const D3D12_RANGE written {0, 0}; r.readback->Unmap(0, &written);
}
static void Collected(const Recording& r)
{
    const auto until = GetTickCount64() + 5000;
    while ((!r.dispatch.expired() || !r.intermediate.expired()) && GetTickCount64() < until) Sleep(1);
    Require(r.dispatch.expired() && r.intermediate.expired(), "invalidated completed owners reclaim without another frame");
}
static Recording UncertainSubmission(ID3D12Device* device, ID3D12CommandQueue* queue,
                                     ID3D12Resource* source, bool batched)
{
    auto shader = std::make_unique<BiasProbe>("RR failed Signal", device);
    Require(shader->IsInit(), "failed-Signal production bias initialized");
    shader->SetRecordedLifetimeEnabled(true);
    auto r = RecordBias(device, *shader, source, .6f);
    Ptr<SignalFailQueue> failing;
    failing.Attach(new SignalFailQueue(queue));
    if (batched) {
        Check(r.list->PrepareUnsplitBatch(failing.Get()), "prepare failed-Signal batch");
        auto* native = r.list->UnsplitNativeList();
        Require(native != nullptr, "batch has real native dispatch");
        failing->ExecuteCommandLists(1, &native);
        r.list->CompleteUnsplitBatch(failing.Get(), S_OK);
    } else {
        Require(r.list->ExecuteOn(failing.Get()) == E_FAIL, "real dispatch completion Signal fails");
    }
    Require(FAILED(r.list->ExecuteOn(queue)), "unknown completion cannot regain proof by replay");
    // A separate queue drain proves these pixels ran, but is not a credential
    // available to the consumer. It must not release its uncertain ownership.
    WaitQueue(device, queue);
    Pixels(r, .6f);
    Ptr<ID3D12CommandAllocator> replacement;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&replacement)), "uncertain replacement allocator");
    Check(r.list->Reset(replacement.Get(), nullptr), "invalidate failed-Signal generation");
    Check(r.list->Close(), "close uncertain replacement");
    shader.reset();
    {
        std::lock_guard lock(Submission::RecordingMutex());
        const auto dispatch = r.dispatch.lock();
        const auto intermediate = r.intermediate.lock();
        Require(dispatch && intermediate, "uncertain dispatch retains full resource closure after helper destruction");
        for (auto* owner : {static_cast<RecordedShader::Owner*>(dispatch.get()),
                                  static_cast<RecordedShader::Owner*>(intermediate.get())}) {
            Require(owner->invalidated && owner->uncertain && owner->submitted,
                    "Signal failure consumer remains fail-closed after invalidation");
            Require(!owner->timer && owner->removalWait && owner->removalEvent && owner->removalFence && owner->module,
                    "uncertain owner has module-pinned one-shot removal wait, never a polling timer");
            Require(owner->removalFence->GetCompletedValue() == 0 && !owner->Complete(),
                    "healthy device and unrelated queue completion cannot retire uncertain owner");
        }
    }
    return r;
}
int main()
{
    spdlog::set_level(spdlog::level::off);
    Ptr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    Ptr<IDXGIFactory4> factory; Ptr<IDXGIAdapter> adapter; Ptr<ID3D12Device> device;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP");
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC qd {};
    Ptr<ID3D12CommandQueue> queue, other;
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&other)), "other queue");
    auto source = Source(device.Get(), queue.Get(), 32, 32);
    auto resized = Source(device.Get(), queue.Get(), 64, 32);

    // Native admission must not emit timing/descriptor/CBV writes or create an intermediate.
    Ptr<ID3D12CommandAllocator> nativeAllocator; Ptr<ID3D12GraphicsCommandList> native;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&nativeAllocator)), "native allocator");
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, nativeAllocator.Get(), nullptr,
                                   IID_PPV_ARGS(&native)), "unobserved native");
    auto shader = std::make_unique<BiasProbe>("RR lifetime", device.Get());
    Require(shader->IsInit(), "production bias initialized"); shader->SetRecordedLifetimeEnabled(true);
    Require(!Shader_Dx12::RecordedLifetimeSupported(native.Get()), "native admission rejected");
    Require(!shader->CreateBufferResource(device.Get(), source.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, native.Get()), "native allocation rejected");
    Require(!shader->Dispatch(native.Get(), source.Get(), .5f, source.Get()) && shader->SlotCount() == 0,
            "native dispatch rejected before allocation or protected writes");
    Check(native->Close(), "native close");

    // More than the legacy cyclic heap count; record without executing, resize,
    // destroy the helper, then submit old recordings in reverse order.
    std::vector<Recording> delayed;
    const float biases[] {.1f, .3f, .5f, .7f, .9f};
    for (unsigned i = 0; i < 5; ++i)
        delayed.push_back(RecordBias(device.Get(), *shader, i == 4 ? resized.Get() : source.Get(), biases[i]));
    shader.reset();
    for (size_t i = delayed.size(); i-- > 0;) {
        Check(delayed[i].list->ExecuteOn(queue.Get()), "delayed execute after helper destruction");
        WaitQueue(device.Get(), queue.Get()); Pixels(delayed[i], biases[i]);
    }
    Check(delayed[0].list->ExecuteOn(other.Get()), "retained recording replay");
    WaitQueue(device.Get(), other.Get()); Pixels(delayed[0], biases[0]);
    for (auto& r : delayed) { r.list.Reset(); Collected(r); }

    // Submitted Reset while blocked behind a queue gate: old generation's GPU
    // references survive the helper, Reset, and replacement recording.
    shader = std::make_unique<BiasProbe>("RR pending", device.Get()); shader->SetRecordedLifetimeEnabled(true);
    auto pending = RecordBias(device.Get(), *shader, source.Get(), .4f);
    Ptr<ID3D12Fence> gate; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate");
    Check(queue->Wait(gate.Get(), 1), "hold pending execution");
    Check(pending.list->ExecuteOn(queue.Get()), "submit blocked execution");
    Ptr<ID3D12CommandAllocator> replacement;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&replacement)), "replacement allocator");
    Check(pending.list->Reset(replacement.Get(), nullptr), "Reset submitted generation");
    shader.reset();
    Require(!pending.dispatch.expired() && !pending.intermediate.expired(), "pending Reset pins complete resource closure");
    Check(pending.list->Close(), "close replacement");
    Check(gate->Signal(1), "release pending execution"); WaitQueue(device.Get(), queue.Get());
    Pixels(pending, .4f); pending.list.Reset(); Collected(pending);

    // Safe slots are reused after discard, not allocated on every ordinary frame.
    shader = std::make_unique<BiasProbe>("RR reuse", device.Get()); shader->SetRecordedLifetimeEnabled(true);
    for (unsigned i = 0; i < 8; ++i) { auto discarded = RecordBias(device.Get(), *shader, source.Get(), .2f); }
    Require(shader->SlotCount() == 1, "discarded generations reuse stable slot");
    shader.reset();

    // Chained dispatches: Bias writes intermediate, OutputScaler consumes it.
    // Cross-helper intermediate link pins the first helper's buffer until both dispatches complete.
    {
        auto rChain = NewRecording(device.Get());
        auto biasChain = std::make_unique<BiasProbe>("Chain Bias", device.Get());
        biasChain->SetRecordedLifetimeEnabled(true);
        Require(biasChain->CreateBufferResource(device.Get(), source.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, rChain.list.Get()), "chain bias buffer");
        auto cleanupBias = biasChain->RecordedBufferCleanup(rChain.list.Get());
        biasChain->SetBufferState(rChain.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Require(biasChain->Dispatch(rChain.list.Get(), source.Get(), 0.5f, biasChain->Buffer()), "chain bias dispatch");
        auto weakBiasInter = biasChain->LastBuffer();

        auto osChain = std::make_unique<OS_Dx12>("Chain OS", device.Get(), false, Scaler::CatmullRom);
        osChain->SetRecordedLifetimeEnabled(true);
        Require(osChain->CreateBufferResource(device.Get(), biasChain->Buffer(), 32, 32, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, rChain.list.Get()), "chain os buffer");
        auto cleanupOS = osChain->RecordedBufferCleanup(rChain.list.Get());
        biasChain->SetBufferState(rChain.list.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        osChain->SetBufferState(rChain.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Require(osChain->Dispatch(rChain.list.Get(), biasChain->Buffer(), osChain->Buffer()), "chain os dispatch");

        // Capture output before running cleanups so final buffer state is COMMON
        osChain->SetBufferState(rChain.list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        Capture(device.Get(), rChain, osChain->Buffer());

        // Execute cleanups in reverse order and clear functors to drop captured CPU owner references
        cleanupOS();
        cleanupBias();
        cleanupOS = nullptr;
        cleanupBias = nullptr;
        Check(rChain.list->Close(), "close chain");

        biasChain.reset(); // First helper destroyed on CPU
        Require(!weakBiasInter.expired(), "intermediate retained by second helper dispatch lease");
        Check(rChain.list->ExecuteOn(queue.Get()), "execute chain");
        WaitQueue(device.Get(), queue.Get());
        rChain.list.Reset();
        osChain.reset(); // Second helper destroyed on CPU after execution
        const auto until = GetTickCount64() + 5000;
        while (!weakBiasInter.expired() && GetTickCount64() < until) Sleep(1);
        Require(weakBiasInter.expired(), "cross-helper intermediate released after GPU execution");
    }

    // Both submission entry points run real production Bias work, then fail
    // only queue Signal. The owners must not arm a healthy-device timer.
    auto failedSignal = UncertainSubmission(device.Get(), queue.Get(), source.Get(), false);
    auto failedBatchSignal = UncertainSubmission(device.Get(), queue.Get(), source.Get(), true);

    Ptr<ID3D12InfoQueue> info;
    if (SUCCEEDED(device.As(&info))) for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0; Check(info->GetMessage(i, nullptr, &bytes), "message length");
        std::vector<char> data(bytes); auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
        Check(info->GetMessage(i, message, &bytes), "debug message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) std::fprintf(stderr, "%s\n", message->pDescription);
        Require(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, "D3D12 debug validation");
    }
    // Removal is the only safe retirement fact for the failed-Signal owners.
    // Do this last: it deliberately makes this WARP device unusable.
    Ptr<ID3D12Device5> removable;
    Check(device.As(&removable), "device-removal test interface");
    removable->RemoveDevice();
    Require(FAILED(device->GetDeviceRemovedReason()), "device removal observed");
    Collected(failedSignal);
    Collected(failedBatchSignal);
    failedSignal.list.Reset();
    failedBatchSignal.list.Reset();
    std::puts("RR post lifetime: PASS (native admission, delayed execution, resize, helper destruction, replay, pending Reset, discard reuse, pixels, Signal failure without polling, device-removal retirement)");
}
