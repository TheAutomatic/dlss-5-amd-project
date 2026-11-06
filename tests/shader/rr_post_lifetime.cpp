// Real production shader dispatches, proxy lifecycle, and readback-visible results.
#include <pch.h>
#include "lmxxf_gpu_test_utils.h"
#include "queue_faults.h"
#include <shaders/bias/Bias_Dx12.h>
#include <shaders/output_scaling/OS_Dx12.h>
#include <shaders/fsrd_preprocess/FSRDCompositionHistory.h>
#include <dlssnr/submission/SubmissionHooks.h>
#include <d3d12sdklayers.h>
#include <array>
#include <vector>
#include <memory>
#include <cmath>
#include <cstring>
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
    void EnableHistoryFinalization()
    {
        Require(_dispatchSlots.empty(), "history root signature selected before recording any dispatch");
        // Match composition's finalizable history SRVs: descriptor identity may
        // change before first submission, but resource data retains the explicit
        // DATA_STATIC_WHILE_SET_AT_EXECUTE promise. Other ranges stay static.
        auto ranges = _descriptorRanges;
        for (auto& range : ranges)
            if (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV)
                range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                              D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        CD3DX12_ROOT_PARAMETER1 parameter {};
        parameter.InitAsDescriptorTable(static_cast<UINT>(ranges.size()), ranges.data());
        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC desc {};
        desc.Init_1_1(1, &parameter);
        Ptr<ID3DBlob> serialized, errors;
        Check(D3D12SerializeVersionedRootSignature(&desc, &serialized, &errors), "history volatile SRV signature");
        Ptr<ID3D12RootSignature> root;
        Check(_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                          IID_PPV_ARGS(&root)), "history volatile SRV root");
        ID3D12PipelineState* pipeline = nullptr;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc {};
        pipelineDesc.pRootSignature = root.Get();
        pipelineDesc.CS = {bias_cso, sizeof(bias_cso)};
        Check(_device->CreateComputePipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)),
              "history Bias pipeline with finalizable root");
        _pipelineState->Release();
        _rootSignature->Release();
        _pipelineState = pipeline;
        _rootSignature = root.Detach();
    }
};
struct Recording
{
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<Submission::CommandListProxy> list;
    Ptr<ID3D12Resource> readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    UINT width = 0, height = 0;
    std::weak_ptr<RecordedShader::Dispatch> dispatch;
    std::shared_ptr<FSRD::CompositionHistory::Use> history;
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
static void Capture(ID3D12Device* device, Recording& r, ID3D12Resource* resource,
                    ID3D12GraphicsCommandList* commands = nullptr)
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
    (commands ? commands : r.list.Get())->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
}
static void RecordBiasCommands(ID3D12Device* device, Recording& r, BiasProbe& shader,
                                ID3D12Resource* source, float bias)
{
    Require(shader.CreateBufferResource(device, source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, r.list.Get()), "owned bias intermediate");
    auto cleanup = shader.RecordedBufferCleanup(r.list.Get());
    Require(bool(cleanup), "exact intermediate cleanup");
    shader.SetBufferState(r.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Require(shader.Dispatch(r.list.Get(), source, bias, shader.Buffer()), "bias dispatch");
    r.dispatch = shader.LastDispatch(); r.intermediate = shader.LastBuffer();
    shader.SetBufferState(r.list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    Capture(device, r, shader.Buffer());
    cleanup();
}
static Recording RecordBias(ID3D12Device* device, BiasProbe& shader, ID3D12Resource* source, float bias)
{
    auto r = NewRecording(device);
    RecordBiasCommands(device, r, shader, source, bias);
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

namespace History = FSRD::CompositionHistory;
struct HistoryReaderUse final : History::Use
{
    std::shared_ptr<RecordedShader::Dispatch> slot;
    Ptr<ID3D12Resource> fallback;
    void* mapped = nullptr;
    UINT fallbackWrites = 0;

    ~HistoryReaderUse() override
    {
        if (mapped) { const D3D12_RANGE written {0, sizeof(float)}; slot->constants->Unmap(0, &written); }
    }
    bool CanFinalizeNoHistory() const noexcept override
    { return slot && mapped && !slot->submitted && !slot->invalidated && slot->identity == identity; }
    void FinalizeNoHistory() noexcept override
    {
        ++fallbackWrites;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = fallback->GetDesc().Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(fallback.Get(), &srv, slot->heap.GetSrvCPU(0));
        *static_cast<float*>(mapped) = .6f;
    }
};
static void ObserveHistory(Recording& r, ID3D12Device* device,
                           const std::shared_ptr<History::Generation>& generation, bool write)
{
    auto use = std::make_shared<History::Use>();
    use->device = device; use->generation = generation; use->write = write;
    Require(use->Attach(r.list.Get()), "production history observer attached");
    if (write) generation->writer = use->identity;
    r.history = use;
}
static Recording HistoryWriter(ID3D12Device* device, BiasProbe& shader, ID3D12Resource* source,
                                const std::shared_ptr<History::Generation>& generation, bool close = true)
{
    auto r = NewRecording(device);
    Require(shader.CreateBufferResource(device, source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, r.list.Get()),
            "history writer allocation");
    shader.SetBufferState(r.list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Require(shader.Dispatch(r.list.Get(), source, .5f, shader.Buffer()), "history writer dispatch");
    auto cleanup = shader.RecordedBufferCleanup(r.list.Get());
    Require(bool(cleanup), "history writer exact buffer cleanup");
    cleanup();
    generation->decisions = shader.Buffer();
    ObserveHistory(r, device, generation, true);
    if (close) Check(r.list->Close(), "close history writer");
    return r;
}
static Recording HistoryReader(ID3D12Device* device, BiasProbe& shader, ID3D12Resource* source,
                                const std::shared_ptr<History::Generation>& generation, bool candidate = true)
{
    std::lock_guard lock(Submission::RecordingMutex());
    auto r = NewRecording(device);
    Require(generation->CanRead(r.list.Get()) == candidate, "history candidate recording policy");
    if (candidate) shader.EnableHistoryFinalization();
    RecordBiasCommands(device, r, shader, candidate ? generation->decisions.Get() : source,
                       candidate ? .4f : .6f);
    if (candidate) {
        auto use = std::make_shared<HistoryReaderUse>();
        use->device = device; use->generation = generation;
        use->slot = r.dispatch.lock(); use->fallback = source;
        Require(bool(use->slot), "history reader owns exact dispatch slot");
        use->slot->resources.emplace_back(source);
        const D3D12_RANGE read {0, 0};
        Check(use->slot->constants->Map(0, &read, &use->mapped), "map private history constants");
        Require(use->Attach(r.list.Get()), "production history finalizer attached");
        r.history = use;
    }
    Check(r.list->Close(), "close history reader");
    return r;
}
static UINT historyBatchCalls = 0, historyBatchLists = 0;
static void ExecuteHistoryBatch(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    ++historyBatchCalls; historyBatchLists += count;
    queue->ExecuteCommandLists(count, lists);
}
static void HistoryDiscard(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* source,
                           bool reset, bool readerBeforeDiscard)
{
    BiasProbe writerShader("History discard writer", device), readerShader("History discard reader", device),
              unrelatedShader("Unrelated batch work", device);
    writerShader.SetRecordedLifetimeEnabled(true); readerShader.SetRecordedLifetimeEnabled(true);
    unrelatedShader.SetRecordedLifetimeEnabled(true);
    auto generation = std::make_shared<History::Generation>();
    auto writer = HistoryWriter(device, writerShader, source, generation);
    Recording reader;
    if (readerBeforeDiscard) reader = HistoryReader(device, readerShader, source, generation);
    if (reset) {
        Check(writer.list->Reset(writer.allocator.Get(), nullptr), "discard history writer by Reset");
        Check(writer.list->Close(), "close reset history writer");
    } else writer.list.Reset();
    Require(generation->discarded && !generation->produced, "discard invalidates unproduced history immediately");
    if (!readerBeforeDiscard) reader = HistoryReader(device, readerShader, source, generation, false);
    auto unrelated = RecordBias(device, unrelatedShader, source, .7f);
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<ID3D12GraphicsCommandList> native;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "batch native allocator");
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&native)), "batch native list");
    Recording nativePixels;
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    native->ResourceBarrier(1, &barrier);
    Capture(device, nativePixels, source, native.Get());
    auto& transition = static_cast<D3D12_RESOURCE_BARRIER&>(barrier).Transition;
    std::swap(transition.StateBefore, transition.StateAfter);
    native->ResourceBarrier(1, &barrier);
    Check(native->Close(), "close batch native list");
    const auto calls = historyBatchCalls, lists = historyBatchLists;
    const auto failures = Submission::g_submissionFailures.load();
    ID3D12CommandList* batch[] { unrelated.list.Get(), native.Get(), reader.list.Get() };
    Submission::Hooks::ExecuteExpanded(queue, 3, batch, nullptr, nullptr, ExecuteHistoryBatch);
    Require(historyBatchCalls == calls + 1 && historyBatchLists == lists + 3,
            "discarded history preserves one complete mixed native batch");
    Require(Submission::g_submissionFailures.load() == failures, "history discard does not reject batch admission");
    WaitQueue(device, queue);
    Pixels(reader, .6f); Pixels(unrelated, .7f); Pixels(nativePixels, 1.f);
    if (readerBeforeDiscard) {
        const auto use = std::static_pointer_cast<HistoryReaderUse>(reader.history);
        Require(use->selection == History::Use::Selection::NoHistory && use->fallbackWrites == 1,
                "discarded candidate finalizes its private descriptors and constants exactly once");
    }
}
static void HistorySubmitted(ID3D12Device* device, ID3D12CommandQueue* queue,
                             ID3D12CommandQueue* other, ID3D12Resource* source)
{
    BiasProbe writerShader("Submitted history writer", device), readerShader("Submitted history reader", device);
    writerShader.SetRecordedLifetimeEnabled(true); readerShader.SetRecordedLifetimeEnabled(true);
    auto generation = std::make_shared<History::Generation>();
    auto writer = HistoryWriter(device, writerShader, source, generation);
    Require(generation->CanRead(writer.list.Get()), "earlier write in the same recording is readable");
    Ptr<ID3D12Fence> gate, readerDone;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "history gate");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&readerDone)), "history reader fence");
    Check(queue->Wait(gate.Get(), 1), "hold submitted history writer");
    Check(writer.list->ExecuteOn(queue), "submit history writer");
    Require(generation->produced && !generation->accesses.empty() &&
            generation->accesses.front().fence->GetCompletedValue() < generation->accesses.front().value,
            "submitted history is published before GPU completion");
    auto reader = HistoryReader(device, readerShader, source, generation, true);
    Ptr<ID3D12CommandAllocator> replacement;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&replacement)),
          "submitted history replacement allocator");
    Check(writer.list->Reset(replacement.Get(), nullptr), "invalidate submitted writer recording");
    Check(writer.list->Close(), "close submitted replacement");
    Require(!generation->discarded, "submitted writer Reset preserves fence-backed history");
    Check(reader.list->ExecuteOn(other), "cross-queue history reader waits for writer proof");
    const auto use = std::static_pointer_cast<HistoryReaderUse>(reader.history);
    Require(use->selection == History::Use::Selection::History && use->fallbackWrites == 0,
            "submitted-pending cross-queue dependency retains history rather than falling back");
    Check(other->Signal(readerDone.Get(), 1), "reader completion sentinel");
    Require(readerDone->GetCompletedValue() == 0, "reader cannot complete before blocked writer");
    Check(gate->Signal(1), "release history writer");
    WaitQueue(device, other);
    Pixels(reader, .2f);
    reader.list.Reset(); writer.list.Reset();
}
static void HistorySameRecording(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* source)
{
    BiasProbe writerShader("Same-recording history writer", device), readerShader("Same-recording history reader", device);
    writerShader.SetRecordedLifetimeEnabled(true); readerShader.SetRecordedLifetimeEnabled(true);
    auto generation = std::make_shared<History::Generation>();
    auto r = HistoryWriter(device, writerShader, source, generation, false);
    Require(generation->CanRead(r.list.Get()), "earlier same-recording write is eligible before submission");
    ObserveHistory(r, device, generation, false);
    RecordBiasCommands(device, r, readerShader, generation->decisions.Get(), .4f);
    Check(r.list->Close(), "close same-recording history");
    Check(r.list->ExecuteOn(queue), "same-recording writer is ordered before reader");
    WaitQueue(device, queue);
    Pixels(r, .2f);
}
static void HistoryOrdered(ID3D12Device* device, ID3D12CommandQueue* queue,
                           ID3D12Resource* source, bool batched)
{
    BiasProbe writerShader("Ordered history writer", device), readerShader("Ordered history reader", device);
    writerShader.SetRecordedLifetimeEnabled(true); readerShader.SetRecordedLifetimeEnabled(true);
    auto generation = std::make_shared<History::Generation>();
    auto writer = HistoryWriter(device, writerShader, source, generation);
    auto reader = HistoryReader(device, readerShader, source, generation);
    const auto use = std::static_pointer_cast<HistoryReaderUse>(reader.history);
    Require(!generation->produced && use->selection == History::Use::Selection::Pending,
            "different-list reader recorded before writer submission retains pending candidate");
    const auto failures = Submission::g_submissionFailures.load();
    if (batched) {
        const auto calls = historyBatchCalls, lists = historyBatchLists;
        ID3D12CommandList* batch[] {writer.list.Get(), reader.list.Get()};
        Submission::Hooks::ExecuteExpanded(queue, 2, batch, nullptr, nullptr, ExecuteHistoryBatch);
        Require(historyBatchCalls == calls + 1 && historyBatchLists == lists + 2,
                "ordered history preserves original two-list Execute batch");
    } else {
        Check(writer.list->ExecuteOn(queue), "submit writer after both lists recorded");
        Check(reader.list->ExecuteOn(queue), "separate ordered submit consumes recorded candidate");
    }
    Require(Submission::g_submissionFailures.load() == failures,
            "valid pre-recorded history never rejects admission");
    Require(use->selection == History::Use::Selection::History && use->fallbackWrites == 0,
            "ordered pre-recorded history retains original CB and SRV");
    WaitQueue(device, queue);
    Pixels(reader, .2f);
}
static std::array<unsigned char, 256> HistoryConstants(const HistoryReaderUse& use)
{
    std::array<unsigned char, 256> bytes {};
    std::memcpy(bytes.data(), use.mapped, bytes.size());
    return bytes;
}
static void HistoryReplay(ID3D12Device* device, ID3D12CommandQueue* queue,
                          ID3D12CommandQueue* other, ID3D12Resource* source, bool readerFirst)
{
    BiasProbe writerShader("Replay history writer", device), readerShader("Replay history reader", device);
    writerShader.SetRecordedLifetimeEnabled(true); readerShader.SetRecordedLifetimeEnabled(true);
    auto generation = std::make_shared<History::Generation>();
    auto writer = HistoryWriter(device, writerShader, source, generation);
    auto reader = HistoryReader(device, readerShader, source, generation);
    const auto use = std::static_pointer_cast<HistoryReaderUse>(reader.history);
    const auto slot = use->slot;
    const auto* heap = slot->heap.GetHeapCSU();
    const auto* constants = slot->constants.Get();
    Ptr<ID3D12Fence> gate, replayDone;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "replay gate");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&replayDone)), "replay completion fence");
    Check(queue->Wait(gate.Get(), 1), "hold first history execution GPU-live");
    if (!readerFirst) Check(writer.list->ExecuteOn(queue), "ordered GPU-live history writer");
    Check(reader.list->ExecuteOn(queue), "first history reader submission");
    const auto selection = readerFirst ? History::Use::Selection::NoHistory : History::Use::Selection::History;
    Require(use->selection == selection && use->fallbackWrites == (readerFirst ? 1u : 0u),
            "first submission latches history choice and private fallback writes");
    const auto bytes = HistoryConstants(*use);
    if (readerFirst) Check(writer.list->ExecuteOn(queue), "late writer does not upgrade no-history reader");
    Check(reader.list->ExecuteOn(other), "GPU-live history reader replay uses immutable slot");
    Check(other->Signal(replayDone.Get(), 1), "GPU-live replay completion sentinel");
    Require(replayDone->GetCompletedValue() == 0, "replay waits for previous GPU-live reader");
    Require(use->selection == selection && use->fallbackWrites == (readerFirst ? 1u : 0u) &&
            HistoryConstants(*use) == bytes && slot->heap.GetHeapCSU() == heap && slot->constants.Get() == constants,
            "GPU-live replay neither rewrites constants/descriptors nor swaps the private slot");
    Check(gate->Signal(1), "release history replay gate");
    WaitQueue(device, queue); WaitQueue(device, other);
    Pixels(reader, readerFirst ? .6f : .2f);
    Check(reader.list->ExecuteOn(other), "completed replay retains first-submission choice");
    WaitQueue(device, other);
    Pixels(reader, readerFirst ? .6f : .2f);
    Require(use->selection == selection && use->fallbackWrites == (readerFirst ? 1u : 0u) &&
            HistoryConstants(*use) == bytes, "completed replay leaves finalized history slot unchanged");
}
static void HistoryPartialRecording(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* source)
{
    BiasProbe firstShader("Partial history first pass", device), failedShader("Partial history rejected pass", device),
              readerShader("Partial history reader", device);
    firstShader.SetRecordedLifetimeEnabled(true); failedShader.SetRecordedLifetimeEnabled(true);
    readerShader.SetRecordedLifetimeEnabled(true);
    auto generation = std::make_shared<History::Generation>();
    auto partial = RecordBias(device, firstShader, source, .5f);
    generation->decisions = firstShader.Buffer();
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<ID3D12GraphicsCommandList> native;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
          "partial history rejected allocator");
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                   IID_PPV_ARGS(&native)), "partial history rejected list");
    Require(!failedShader.Dispatch(native.Get(), source, .4f, generation->decisions.Get()),
            "later composition-like pass really fails recorded-lifetime admission");
    Check(native->Close(), "close rejected partial pass");
    // Production attaches the writer only after every composition dispatch was
    // admitted. Exercise that public contract, without claiming to fault-inject
    // the private multi-pass DispatchComposition implementation in this target.
    Require(!generation->writer.list && !generation->CanRead(partial.list.Get()),
            "allocated/partially written resources are not a published history candidate");
    auto reader = HistoryReader(device, readerShader, source, generation, false);
    ID3D12CommandList* batch[] {partial.list.Get(), reader.list.Get()};
    Submission::Hooks::ExecuteExpanded(queue, 2, batch, nullptr, nullptr, ExecuteHistoryBatch);
    WaitQueue(device, queue);
    Require(!generation->produced && !generation->writer.list,
            "executing partial work cannot publish history without a committed writer observer");
    Pixels(partial, .5f); Pixels(reader, .6f);
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

    // Enhanced barriers recorded by the title or FFX denoiser/upscaler on the
    // same CommandListProxy must not reject RR conversion/composition/postprocessing.
    {
        auto enhanced = NewRecording(device.Get());
        D3D12_GLOBAL_BARRIER global {
            D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_ALL,
            D3D12_BARRIER_ACCESS_COMMON, D3D12_BARRIER_ACCESS_COMMON
        };
        D3D12_BARRIER_GROUP group {};
        group.Type = D3D12_BARRIER_TYPE_GLOBAL;
        group.NumBarriers = 1;
        group.pGlobalBarriers = &global;
        enhanced.list->Barrier(1, &group);
        Require(!enhanced.list->CanAppendCompute(), "enhanced barriers still block legacy shared output effects");
        Require(enhanced.list->CanRecordOwnedCompute() && Shader_Dx12::RecordedLifetimeSupported(enhanced.list.Get()),
                "enhanced barriers do not reject RR recorded-lifetime compute helpers");
        RecordBiasCommands(device.Get(), enhanced, *shader, source.Get(), .5f);
        Check(enhanced.list->Close(), "close enhanced-barrier recording");
        Check(enhanced.list->ExecuteOn(queue.Get()), "execute enhanced-barrier recording");
        WaitQueue(device.Get(), queue.Get());
        Pixels(enhanced, .5f);
    }

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

    // Published-but-unsubmitted history must not make any unrelated game list
    // disappear. Cover Reset/final Release on both sides of reader recording.
    for (const bool reset : {false, true})
        for (const bool readerBeforeDiscard : {false, true})
            HistoryDiscard(device.Get(), queue.Get(), source.Get(), reset, readerBeforeDiscard);
    HistorySubmitted(device.Get(), queue.Get(), other.Get(), source.Get());
    HistorySameRecording(device.Get(), queue.Get(), source.Get());
    for (const bool batched : {false, true})
        HistoryOrdered(device.Get(), queue.Get(), source.Get(), batched);
    for (const bool readerFirst : {false, true})
        HistoryReplay(device.Get(), queue.Get(), other.Get(), source.Get(), readerFirst);
    HistoryPartialRecording(device.Get(), queue.Get(), source.Get());

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
    std::puts("RR post lifetime: PASS (native admission, delayed execution, resize, helper destruction, replay, pending Reset, discard reuse, history writer Reset/Release before/after reader, native/unrelated batch pixels, ordered cross-list history separate/batched, cross-queue pending history, immutable GPU-live history/no-history replay, partial recording publication contract, Signal failure without polling, device-removal retirement)");
}
