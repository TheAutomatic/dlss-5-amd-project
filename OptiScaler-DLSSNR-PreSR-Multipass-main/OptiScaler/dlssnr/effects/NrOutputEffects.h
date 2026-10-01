#pragma once
#include "../NrEffectsSettings.h"
#include "../submission/CommandListProxy.h"
#include "../backend/lmxxf_runtime/RecordingGpuTiming.h"
#include "../../../../third_party/lmxxf/src/native_shader_cache.h"
#include <wrl/client.h>
#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace DlssNr::Effects
{
using Microsoft::WRL::ComPtr;
inline constexpr char BlendShader[] = R"(
Texture2D<float4> original:register(t0);
Texture2D<float4> result:register(t1);
RWTexture2D<float4> output:register(u0);
cbuffer Params:register(b0){uint width,height;float intensity;uint reserved;}
[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){
 if(p.x>=width||p.y>=height)return;
 float4 b=original.Load(int3(p.xy,0)),r=result.Load(int3(p.xy,0));
 if(!all(isfinite(b)))b=0;
 if(!all(isfinite(r)))r=b;
 float3 c=lerp(b.rgb,r.rgb,min(intensity,1));
 if(intensity>1){
  float3 d=r.rgb-b.rgb;
  // Bound only the extra extrapolation; ordinary attenuation stays linear.
  float3 limit=.5*max(max(abs(b.rgb),abs(r.rgb)),.001);
  c=r.rgb+clamp(d,-limit,limit)*(intensity-1);
 }
 output[p.xy]=float4(clamp(c,-65504,65504),b.a);
}
)";
inline DXGI_FORMAT ReadFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}
inline bool ColorFormat(DXGI_FORMAT f)
{
    switch (ReadFormat(f)) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return true;
    default: return false;
    }
}
inline void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}
struct Pipeline
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> blend;
    static std::shared_ptr<Pipeline> Create(ID3D12Device* device)
    {
        auto p = std::make_shared<Pipeline>(); p->device = device;
        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2};
        D3D12_ROOT_PARAMETER params[2] {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {2, ranges};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = {0, 0, 4};
        D3D12_ROOT_SIGNATURE_DESC desc {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> root, error, code;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &root, &error)) ||
            FAILED(device->CreateRootSignature(0, root->GetBufferPointer(), root->GetBufferSize(), IID_PPV_ARGS(&p->root)))) return {};
        if (FAILED(NativeCompileShaderBlob(BlendShader, sizeof BlendShader - 1, "NR overall intensity", nullptr,
                                           nullptr, "main", &code, &error))) return {};
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline {}; pipeline.pRootSignature = p->root.Get();
        pipeline.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        if (FAILED(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&p->blend)))) return {};
        return p;
    }
};
struct Storage
{
    std::shared_ptr<Pipeline> pipeline;
    ComPtr<ID3D12Resource> output;
    ComPtr<ID3D12DescriptorHeap> heap;
    std::shared_ptr<LmxxfRuntime::RecordingGpuTiming> timing;
    UINT width = 0, height = 0;
    UINT64 bytes = 0;
    static D3D12_RESOURCE_DESC Description(UINT width, UINT height)
    {
        D3D12_RESOURCE_DESC desc {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width; desc.Height = height; desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return desc;
    }
    static std::shared_ptr<Storage> Create(std::shared_ptr<Pipeline> pipeline, UINT width, UINT height)
    {
        auto s = std::make_shared<Storage>(); s->pipeline = std::move(pipeline); s->width = width; s->height = height;
        const auto desc = Description(width, height);
        s->bytes = s->pipeline->device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(s->pipeline->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&s->output)))) return {};
        D3D12_DESCRIPTOR_HEAP_DESC hd {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(s->pipeline->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s->heap)))) return {};
        return s;
    }
};
struct Lease;
struct State
{
    std::vector<std::shared_ptr<Lease>> leases;
    std::vector<std::shared_ptr<Storage>> storage;
    std::shared_ptr<Pipeline> pipeline;
    PerformanceStore performance;
    std::mutex statusMutex;
    std::string status;
    void Status(const char* value) { std::lock_guard lock(statusMutex); status = value; }
};
inline State& Global() { static auto* state = new State; return *state; }
inline bool CollectLocked();
inline void ScheduleCollectionLocked() noexcept;
struct Lease final : Submission::RecordingObserver
{
    std::shared_ptr<Storage> storage;
    ComPtr<ID3D12Resource> original, result;
    Submission::RecordingIdentity identity;
    std::vector<std::shared_ptr<LmxxfRuntime::RecordingCompletion>> completions;
    bool invalidated = false, unconfirmed = false, continuation = false;
    uint64_t frame = 0;
    HRESULT BeforeExecute(const Submission::RecordingExecution& e) noexcept override
    {
        if (invalidated || !(e.identity == identity) || unconfirmed) return E_UNEXPECTED;
        completions.erase(std::remove_if(completions.begin(), completions.end(), [](const auto& p) { return p->Complete(); }), completions.end());
        if (storage->timing) storage->timing->BeforeExecution(Global().performance);
        return S_OK;
    }
    void Executed(const Submission::RecordingExecution& e) noexcept override
    {
        if (!e.producerSubmitted) return;
        unconfirmed = true;
        if (FAILED(e.status) || !e.fence || !e.fenceValue || e.fenceValue == UINT64_MAX) return;
        try {
            auto proof = std::make_shared<LmxxfRuntime::RecordingCompletion>(e.fence, e.queue, e.fenceValue);
            completions.push_back(proof);
            if (storage->timing && (!continuation || e.continuationSubmitted) && Global().performance.Enabled())
                storage->timing->Submitted(proof, true, frame, e.serial, Global().performance.Epoch());
            unconfirmed = false;
        } catch (...) {} // Keep the registry lease on allocation failure after submission.
    }
    void Invalidated(Submission::RecordingIdentity id) noexcept override
    {
        if (!(id == identity)) return;
        invalidated = true;
        if (CollectLocked()) ScheduleCollectionLocked();
    }
};
inline bool CollectLocked()
{
    auto& s = Global(); bool pending = false;
    for (auto it = s.leases.begin(); it != s.leases.end();) {
        auto& lease = *it;
        const bool removed = FAILED(lease->storage->pipeline->device->GetDeviceRemovedReason());
        if (!removed && lease->storage->timing) lease->storage->timing->Collect(s.performance);
        auto& proofs = lease->completions;
        proofs.erase(std::remove_if(proofs.begin(), proofs.end(), [](const auto& p) { return p->Complete(); }), proofs.end());
        if (lease->invalidated && (removed || (!lease->unconfirmed && proofs.empty()))) it = s.leases.erase(it);
        else { pending |= lease->invalidated; ++it; }
    }
    return pending;
}
struct TimerState { PTP_TIMER timer = nullptr; HMODULE module = nullptr; };
inline TimerState& Timer() { static TimerState state; return state; }
inline void Arm(PTP_TIMER timer) noexcept
{
    LARGE_INTEGER due {}; due.QuadPart = -1000000;
    FILETIME when {due.LowPart, DWORD(due.HighPart)}; SetThreadpoolTimer(timer, &when, 0, 0);
}
inline void CALLBACK CollectionCallback(PTP_CALLBACK_INSTANCE instance, void*, PTP_TIMER timer)
{
    std::lock_guard lock(Submission::RecordingMutex());
    if (CollectLocked()) { Arm(timer); return; }
    auto module = Timer().module; Timer() = {}; CloseThreadpoolTimer(timer);
    FreeLibraryWhenCallbackReturns(instance, module);
}
inline void ScheduleCollectionLocked() noexcept
{
    if (Timer().timer) return;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&CollectionCallback), &module)) return;
    auto* timer = CreateThreadpoolTimer(&CollectionCallback, nullptr, nullptr);
    if (!timer) { FreeLibrary(module); return; }
    Timer() = {timer, module}; Arm(timer);
}
inline void Poll()
{
    std::lock_guard lock(Submission::RecordingMutex());
    if (CollectLocked()) ScheduleCollectionLocked();
}
inline std::string Status() { auto& s = Global(); std::lock_guard lock(s.statusMutex); return s.status; }
inline NrTimingSnapshot Timing() { return Global().performance.Read(); }
inline void Reset()
{
    std::lock_guard lock(Submission::RecordingMutex());
    auto& s = Global(); s.storage.clear(); s.pipeline.reset(); s.performance.SetEnabled(false); s.Status("");
    if (CollectLocked()) ScheduleCollectionLocked();
}
inline ID3D12Resource* Record(ID3D12GraphicsCommandList* cmd, ID3D12Resource* original, ID3D12Resource* result,
                              D3D12_RESOURCE_STATES originalState, UINT width, UINT height,
                              float requestedIntensity, bool timingEnabled)
{
    const float intensity = OverallIntensity(requestedIntensity);
    if (!cmd || !original || !result) return result;
    std::lock_guard lock(Submission::RecordingMutex());
    auto& s = Global();
    s.performance.SetEnabled(timingEnabled && intensity != 0 && intensity != 1 && original != result);
    CollectLocked();
    if (intensity == 1 || original == result) { s.storage.clear(); s.Status(""); return result; }
    if (intensity == 0) { s.storage.clear(); s.Status("Overall Intensity: original image (NR still runs)"); return original; }
    try {
        ComPtr<Submission::ILogicalCommandList> logical;
        ComPtr<Submission::IRecordingResources> observer;
        if (FAILED(cmd->QueryInterface(IID_PPV_ARGS(&logical))) || FAILED(cmd->QueryInterface(IID_PPV_ARGS(&observer)))) {
            s.Status("Overall Intensity unavailable: restart to enable recording ownership"); return result;
        }
        if (!observer->CanAppendCompute()) { s.Status("Overall Intensity bypassed: unsupported command-list state"); return result; }
        const auto a = original->GetDesc(), b = result->GetDesc();
        if (!width || !height || a.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            b.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || a.SampleDesc.Count != 1 || b.SampleDesc.Count != 1 ||
            a.DepthOrArraySize != 1 || b.DepthOrArraySize != 1 ||
            (a.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || (b.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || a.Width < width || b.Width < width ||
            a.Height < height || b.Height < height || !ColorFormat(a.Format) || !ColorFormat(b.Format)) {
            s.Status("Overall Intensity bypassed: unsupported colour input"); return result;
        }
        ComPtr<ID3D12Device> device; if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&device)))) return result;
        if (!s.pipeline || s.pipeline->device.Get() != device.Get()) {
            s.storage.clear(); s.pipeline = Pipeline::Create(device.Get());
            if (!s.pipeline) { s.Status("Overall Intensity unavailable: shader initialization failed"); return result; }
        }
        s.storage.erase(std::remove_if(s.storage.begin(), s.storage.end(), [&](const auto& item) {
            return item.use_count() == 1 && (item->width != width || item->height != height);
        }), s.storage.end());
        // Budget counts retained recordings too, not just the current reusable pool.
        std::vector<const Storage*> seen; UINT64 bytes = 0;
        auto count = [&](const std::shared_ptr<Storage>& item) {
            if (std::find(seen.begin(), seen.end(), item.get()) == seen.end()) { seen.push_back(item.get()); bytes += item->bytes; }
        };
        for (auto& item : s.storage) count(item);
        for (auto& lease : s.leases) count(lease->storage);
        std::shared_ptr<Storage> storage;
        for (auto& item : s.storage) if (item.use_count() == 1 && item->width == width && item->height == height) { storage = item; break; }
        if (!storage) {
            constexpr UINT64 budget = 512ull * 1024 * 1024;
            const auto desc = Storage::Description(width, height);
            const UINT64 estimate = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
            if (seen.size() >= 16 || bytes > budget || estimate > budget - bytes) {
                s.Status("Overall Intensity bypassed: recording resource budget in use"); return result;
            }
            storage = Storage::Create(s.pipeline, width, height);
            if (!storage) { s.Status("Overall Intensity unavailable: resource allocation failed"); return result; }
            s.storage.push_back(storage);
        }
        auto lease = std::make_shared<Lease>(); lease->storage = storage; lease->original = original; lease->result = result;
        lease->identity = logical->Identity(); lease->continuation = observer->InContinuation();
        lease->frame = lease->identity.generation;
        if (timingEnabled && !storage->timing)
            storage->timing = LmxxfRuntime::RecordingGpuTiming::Create(device.Get(), NR_GPU_BLEND, NR_GPU_STABILIZER);
        if (storage->timing) storage->timing->ResetRecording();
        s.leases.push_back(lease);
        if (FAILED(observer->ObserveResources(lease))) { lease->invalidated = true; CollectLocked(); return result; }
        const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = storage->heap->GetCPUDescriptorHandleForHeapStart();
        for (auto* resource : {original, result}) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view {}; view.Format = ReadFormat(resource->GetDesc().Format);
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1; device->CreateShaderResourceView(resource, &view, cpu); cpu.ptr += stride;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC output {}; output.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        output.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(storage->output.Get(), nullptr, &output, cpu);
        if (timingEnabled && storage->timing) storage->timing->Begin(cmd, 0);
        const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        Barrier(cmd, original, originalState, read);
        Barrier(cmd, storage->output.Get(), read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto* heap = storage->heap.Get(); cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootSignature(s.pipeline->root.Get()); cmd->SetPipelineState(s.pipeline->blend.Get());
        cmd->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        struct { UINT width, height; float intensity; UINT reserved; } constants {width, height, intensity, 0};
        cmd->SetComputeRoot32BitConstants(1, 4, &constants, 0); cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        Barrier(cmd, storage->output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
        Barrier(cmd, original, read, originalState);
        if (timingEnabled && storage->timing) storage->timing->End(cmd, 0);
        s.Status("Overall Intensity active"); return storage->output.Get();
    } catch (...) { s.Status("Overall Intensity bypassed: preparation failed"); return result; }
}
}
