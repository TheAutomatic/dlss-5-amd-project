#pragma once
// Shared SR-output adapter. Runtime ABIs remain unchanged: each backend receives
// one colour-sized guide grid and returns its ordinary FP16 private output.
#include "amd/AmdPreSr.h"
#include "effects/NrOutputEffects.h"

namespace DlssNr::PostSr
{
using Effects::Barrier;
using Microsoft::WRL::ComPtr;
inline constexpr auto Read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
inline constexpr auto Write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
inline constexpr char GuideShader[] = R"(
Texture2D<float2> motion:register(t0);
Texture2D<float> depth:register(t1);
RWTexture2D<float2> outMotion:register(u0);
RWTexture2D<float> outDepth:register(u1);
cbuffer Params:register(b0){uint width,height,mw,mh,dw,dh;float jx,jy;}
[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){
 if(p.x>=width||p.y>=height)return;
 // NGX jitter is in render pixels. SR colour is unjittered; sample the
 // corresponding jittered guide location, independently of allocation padding.
 float2 uv=(float2(p.xy)+.5)/float2(width,height)+float2(jx,jy)/float2(dw,dh);
 int2 mp=clamp(int2(floor(uv*float2(mw,mh))),int2(0,0),int2(mw-1,mh-1));
 int2 dp=clamp(int2(floor(uv*float2(dw,dh))),int2(0,0),int2(dw-1,dh-1));
 outMotion[p.xy]=motion.Load(int3(mp,0));
 outDepth[p.xy]=depth.Load(int3(dp,0));
}
)";
inline constexpr char CopyShader[] = R"(
Texture2D<float4> result:register(t0);
Texture2D<float4> original:register(t1);
RWTexture2D<float4> output:register(u0);
cbuffer Params:register(b0){uint width,height;uint2 reserved;uint4 padding;}
[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){
 if(p.x>=width||p.y>=height)return;
 float4 b=original.Load(int3(p.xy,0)),r=result.Load(int3(p.xy,0));
 output[p.xy]=float4(all(isfinite(r.rgb))?clamp(r.rgb,-65504,65504):b.rgb,b.a);
}
)";
inline DXGI_FORMAT OutputFormat(DXGI_FORMAT f)
{
    switch (Effects::ReadFormat(f))
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return Effects::ReadFormat(f);
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}
inline bool TextureExtent(ID3D12Resource* r, UINT w, UINT h)
{
    if (!r || !w || !h)
        return false;
    const auto d = r->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1 &&
           d.Width >= w && d.Height >= h && !(d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
}
struct Pipeline
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> guides, copy;
    static std::shared_ptr<Pipeline> Create(ID3D12Device* d)
    {
        auto p = std::make_shared<Pipeline>();
        p->device = d;
        D3D12_DESCRIPTOR_RANGE ranges[] { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0 },
                                          { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 2 } };
        D3D12_ROOT_PARAMETER args[2] {};
        args[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        args[0].DescriptorTable = { 2, ranges };
        args[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        args[1].Constants = { 0, 0, 8 };
        D3D12_ROOT_SIGNATURE_DESC desc { 2, args, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> root, error;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &root, &error)) ||
            FAILED(d->CreateRootSignature(0, root->GetBufferPointer(), root->GetBufferSize(), IID_PPV_ARGS(&p->root))))
            return {};
        auto compile = [&](const char* source, size_t size, ID3D12PipelineState** out)
        {
            ComPtr<ID3DBlob> code;
            if (FAILED(NativeCompileShaderBlob(source, size, "Post-SR NR", nullptr, nullptr, "main", &code, &error)))
                return false;
            D3D12_COMPUTE_PIPELINE_STATE_DESC ps {};
            ps.pRootSignature = p->root.Get();
            ps.CS = { code->GetBufferPointer(), code->GetBufferSize() };
            return SUCCEEDED(d->CreateComputePipelineState(&ps, IID_PPV_ARGS(out)));
        };
        if (!compile(GuideShader, sizeof GuideShader - 1, &p->guides) ||
            !compile(CopyShader, sizeof CopyShader - 1, &p->copy))
            return {};
        return p;
    }
};
struct Storage
{
    std::shared_ptr<Pipeline> pipeline;
    ComPtr<ID3D12Resource> motion, depth, output;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT64 bytes = 0;
    static std::shared_ptr<Storage> Create(std::shared_ptr<Pipeline> p, UINT w, UINT h, DXGI_FORMAT format)
    {
        auto s = std::make_shared<Storage>();
        s->pipeline = p;
        s->width = w;
        s->height = h;
        s->format = format;
        auto make = [&](DXGI_FORMAT f, ID3D12Resource** out)
        {
            auto desc = Effects::Storage::Description(w, h);
            desc.Format = f;
            s->bytes += p->device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
            D3D12_HEAP_PROPERTIES hp {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            return SUCCEEDED(
                p->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, Read, nullptr, IID_PPV_ARGS(out)));
        };
        if (!make(DXGI_FORMAT_R32G32_FLOAT, &s->motion) || !make(DXGI_FORMAT_R32_FLOAT, &s->depth) ||
            !make(format, &s->output))
            return {};
        D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
        if (FAILED(p->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s->heap))))
            return {};
        return s;
    }
};
struct Lease;
struct State
{
    std::shared_ptr<Pipeline> pipeline;
    std::vector<std::shared_ptr<Storage>> pool;
    std::vector<std::shared_ptr<Lease>> leases;
};
inline State& Global()
{
    static auto* s = new State;
    return *s;
}
inline bool CollectLocked();
inline void ScheduleCollectionLocked() noexcept;
struct Lease final : Submission::RecordingObserver
{
    std::shared_ptr<Storage> storage;
    ComPtr<ID3D12Resource> target, motion, depth, result;
    Submission::RecordingIdentity identity;
    std::vector<std::shared_ptr<LmxxfRuntime::RecordingCompletion>> completions;
    bool invalidated = false, unconfirmed = false;
    HRESULT BeforeExecute(const Submission::RecordingExecution& e) noexcept override
    {
        if (invalidated || unconfirmed || !(identity == e.identity))
            return E_UNEXPECTED;
        // A closed list may be replayed on a different queue. Serialize writers
        // to this recording's private guides/output until their previous use ends.
        for (const auto& p : completions)
            if (p->queue != e.queue && !p->Complete())
            {
                const auto hr = e.queue->Wait(p->fence, p->value);
                if (FAILED(hr))
                    return hr;
            }
        completions.erase(std::remove_if(completions.begin(), completions.end(), [](auto& p) { return p->Complete(); }),
                          completions.end());
        return S_OK;
    }
    void Executed(const Submission::RecordingExecution& e) noexcept override
    {
        if (!e.producerSubmitted)
            return;
        unconfirmed = true;
        if (FAILED(e.status) || !e.fence || !e.fenceValue || e.fenceValue == UINT64_MAX)
            return;
        try
        {
            completions.push_back(std::make_shared<LmxxfRuntime::RecordingCompletion>(e.fence, e.queue, e.fenceValue));
            unconfirmed = false;
        }
        catch (...)
        {
        }
    }
    void Invalidated(Submission::RecordingIdentity id) noexcept override
    {
        if (!(identity == id))
            return;
        invalidated = true;
        if (CollectLocked())
            ScheduleCollectionLocked();
    }
};
inline bool CollectLocked()
{
    auto& s = Global();
    bool pending = false;
    for (auto it = s.leases.begin(); it != s.leases.end();)
    {
        auto& l = *it;
        const bool removed = FAILED(l->storage->pipeline->device->GetDeviceRemovedReason());
        auto& c = l->completions;
        c.erase(std::remove_if(c.begin(), c.end(), [](auto& p) { return p->Complete(); }), c.end());
        if (l->invalidated && (removed || (!l->unconfirmed && c.empty())))
            it = s.leases.erase(it);
        else
        {
            pending |= l->invalidated;
            ++it;
        }
    }
    return pending;
}
struct TimerState
{
    PTP_TIMER timer = nullptr;
    HMODULE module = nullptr;
};
inline TimerState& Timer()
{
    static TimerState t;
    return t;
}
inline void Arm(PTP_TIMER t)
{
    LARGE_INTEGER due {};
    due.QuadPart = -1000000;
    FILETIME f { due.LowPart, DWORD(due.HighPart) };
    SetThreadpoolTimer(t, &f, 0, 0);
}
inline void CALLBACK CollectionCallback(PTP_CALLBACK_INSTANCE instance, void*, PTP_TIMER timer)
{
    std::lock_guard lock(Submission::RecordingMutex());
    if (CollectLocked())
    {
        Arm(timer);
        return;
    }
    const auto module = Timer().module;
    Timer() = {};
    CloseThreadpoolTimer(timer);
    FreeLibraryWhenCallbackReturns(instance, module);
}
inline void ScheduleCollectionLocked() noexcept
{
    if (Timer().timer)
        return;
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&CollectionCallback), &m))
        return;
    auto* t = CreateThreadpoolTimer(&CollectionCallback, nullptr, nullptr);
    if (!t)
    {
        FreeLibrary(m);
        return;
    }
    Timer() = { t, m };
    Arm(t);
}
inline void Poll()
{
    std::lock_guard lock(Submission::RecordingMutex());
    if (CollectLocked())
        ScheduleCollectionLocked();
}
inline void Reset()
{
    std::lock_guard lock(Submission::RecordingMutex());
    Global().pool.clear();
    Global().pipeline.reset();
    if (CollectLocked())
        ScheduleCollectionLocked();
}

// Prepare allocations and register ownership BEFORE recording any GPU command.
// Failure leaves the SR output intact and never falls back to running NR before SR.
inline std::shared_ptr<Lease> Prepare(ID3D12GraphicsCommandList* cmd, AmdPreSr::Frame& f, UINT renderWidth,
                                      UINT renderHeight, std::string& reason)
{
    reason = "SR -> NR: unsupported output or guide layout";
    if (!cmd || !TextureExtent(f.colour, f.width, f.height) || !TextureExtent(f.depth, renderWidth, renderHeight))
        return {};
    const UINT mw = f.motionWidth ? f.motionWidth : renderWidth, mh = f.motionHeight ? f.motionHeight : renderHeight;
    const auto format = OutputFormat(f.colour->GetDesc().Format);
    if (format == DXGI_FORMAT_UNKNOWN || !TextureExtent(f.motion, mw, mh) ||
        Effects::DepthFormat(f.depth->GetDesc().Format) == DXGI_FORMAT_UNKNOWN ||
        Effects::MotionFormat(f.motion->GetDesc().Format) == DXGI_FORMAT_UNKNOWN || f.colour == f.depth ||
        f.colour == f.motion || f.depth == f.motion || !std::isfinite(f.jitterX) || !std::isfinite(f.jitterY) ||
        !std::isfinite(f.motionScaleX) || !std::isfinite(f.motionScaleY))
        return {};
    std::lock_guard lock(Submission::RecordingMutex());
    try
    {
        ComPtr<Submission::ILogicalCommandList> logical;
        ComPtr<Submission::IRecordingResources> observer;
        if (FAILED(cmd->QueryInterface(IID_PPV_ARGS(&logical))) || FAILED(cmd->QueryInterface(IID_PPV_ARGS(&observer))))
        {
            reason = "SR -> NR: Save Settings and restart to enable command-list ownership";
            return {};
        }
        if (!observer->CanAppendCompute())
        {
            reason = "SR -> NR: unsupported command-list state this frame";
            return {};
        }
        ComPtr<ID3D12Device> d;
        if (FAILED(cmd->GetDevice(IID_PPV_ARGS(&d))))
            return {};
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support { format };
        if (FAILED(d->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support)) ||
            !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            return {};
        auto& s = Global();
        CollectLocked();
        if (!s.pipeline || s.pipeline->device.Get() != d.Get())
        {
            s.pool.clear();
            s.pipeline = Pipeline::Create(d.Get());
        }
        if (!s.pipeline)
        {
            reason = "SR -> NR: shader initialization failed";
            return {};
        }
        s.pool.erase(std::remove_if(s.pool.begin(), s.pool.end(),
                                    [&](auto& a)
                                    {
                                        return a.use_count() == 1 &&
                                               (a->width != f.width || a->height != f.height || a->format != format);
                                    }),
                     s.pool.end());
        std::shared_ptr<Storage> storage;
        for (auto& a : s.pool)
            if (a.use_count() == 1)
            {
                storage = a;
                break;
            }
        if (!storage)
        {
            std::vector<const Storage*> seen;
            UINT64 bytes = 0;
            auto count = [&](auto& a)
            {
                if (std::find(seen.begin(), seen.end(), a.get()) == seen.end())
                {
                    seen.push_back(a.get());
                    bytes += a->bytes;
                }
            };
            for (auto& a : s.pool)
                count(a);
            for (auto& l : s.leases)
                count(l->storage);
            UINT64 estimate = 0;
            for (auto fmt : { DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32_FLOAT, format })
            {
                auto desc = Effects::Storage::Description(f.width, f.height);
                desc.Format = fmt;
                const auto n = d->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
                if (n == UINT64_MAX)
                    return {};
                estimate += n;
            }
            constexpr UINT64 budget = 768ull * 1024 * 1024;
            if (seen.size() >= 16 || bytes > budget || estimate > budget - bytes)
            {
                reason = "SR -> NR: adapter memory budget in use; lower resolution or wait for recordings";
                return {};
            }
            storage = Storage::Create(s.pipeline, f.width, f.height, format);
            if (!storage)
            {
                reason = "SR -> NR: texture allocation failed";
                return {};
            }
            s.pool.push_back(storage);
        }
        auto l = std::make_shared<Lease>();
        l->storage = storage;
        l->target = f.colour;
        l->motion = f.motion;
        l->depth = f.depth;
        l->identity = logical->Identity();
        s.leases.push_back(l);
        if (FAILED(observer->ObserveResources(l)))
        {
            l->invalidated = true;
            CollectLocked();
            return {};
        }
        const UINT stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu = storage->heap->GetCPUDescriptorHandleForHeapStart();
        auto srv = [&](ID3D12Resource* r, DXGI_FORMAT fmt)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC v {};
            v.Format = fmt;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.Texture2D.MipLevels = 1;
            d->CreateShaderResourceView(r, &v, cpu);
            cpu.ptr += stride;
        };
        auto uav = [&](ID3D12Resource* r, DXGI_FORMAT fmt)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
            v.Format = fmt;
            v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            d->CreateUnorderedAccessView(r, nullptr, &v, cpu);
            cpu.ptr += stride;
        };
        srv(f.motion, Effects::MotionFormat(f.motion->GetDesc().Format));
        srv(f.depth, Effects::DepthFormat(f.depth->GetDesc().Format));
        uav(storage->motion.Get(), DXGI_FORMAT_R32G32_FLOAT);
        uav(storage->depth.Get(), DXGI_FORMAT_R32_FLOAT);
        // Reserve a separate immutable descriptor range for final composition.
        cpu.ptr += stride;
        srv(f.colour, Effects::ReadFormat(f.colour->GetDesc().Format));
        uav(storage->output.Get(), format);
        uav(nullptr, DXGI_FORMAT_R32_FLOAT);
        Barrier(cmd, f.motion, f.motionState, Read);
        Barrier(cmd, f.depth, f.depthState, Read);
        Barrier(cmd, storage->motion.Get(), Read, Write);
        Barrier(cmd, storage->depth.Get(), Read, Write);
        auto* heap = storage->heap.Get();
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootSignature(s.pipeline->root.Get());
        cmd->SetPipelineState(s.pipeline->guides.Get());
        cmd->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
        struct
        {
            UINT w, h, mw, mh, dw, dh;
            float jx, jy;
        } constants { f.width, f.height, mw, mh, renderWidth, renderHeight, f.jitterX, f.jitterY };
        cmd->SetComputeRoot32BitConstants(1, 8, &constants, 0);
        cmd->Dispatch((f.width + 7) / 8, (f.height + 7) / 8, 1);
        Barrier(cmd, storage->motion.Get(), Write, Read);
        Barrier(cmd, storage->depth.Get(), Write, Read);
        Barrier(cmd, f.motion, Read, f.motionState);
        Barrier(cmd, f.depth, Read, f.depthState);
        f.motion = storage->motion.Get();
        f.depth = storage->depth.Get();
        f.motionState = f.depthState = Read;
        f.motionScaleX *= float(f.width) / mw;
        f.motionScaleY *= float(f.height) / mh;
        f.motionWidth = f.width;
        f.motionHeight = f.height;
        f.jitterX = f.jitterY = 0;
        reason.clear();
        return l;
    }
    catch (...)
    {
        reason = "SR -> NR: preparation failed";
        return {};
    }
}
inline bool Finish(ID3D12GraphicsCommandList* cmd, const std::shared_ptr<Lease>& l, ID3D12Resource* result,
                   D3D12_RESOURCE_STATES targetState)
{
    if (!l || !result)
        return false;
    if (result == l->target.Get())
        return true; // Overall intensity zero: preserve SR exactly.
    auto& s = *l->storage;
    if (!TextureExtent(result, s.width, s.height) || !Effects::ColorFormat(result->GetDesc().Format))
        return false;
    l->result = result;
    auto* d = s.pipeline->device.Get();
    const auto stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = s.heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += 4 * stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC v {};
    v.Format = Effects::ReadFormat(result->GetDesc().Format);
    v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    v.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(result, &v, cpu);
    Barrier(cmd, l->target.Get(), targetState, Read);
    Barrier(cmd, s.output.Get(), Read, Write);
    auto* heap = s.heap.Get();
    cmd->SetDescriptorHeaps(1, &heap);
    cmd->SetComputeRootSignature(s.pipeline->root.Get());
    cmd->SetPipelineState(s.pipeline->copy.Get());
    auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += 4 * stride;
    cmd->SetComputeRootDescriptorTable(0, gpu);
    UINT dims[] { s.width, s.height, 0, 0, 0, 0, 0, 0 };
    cmd->SetComputeRoot32BitConstants(1, 8, dims, 0);
    cmd->Dispatch((s.width + 7) / 8, (s.height + 7) / 8, 1);
    Barrier(cmd, s.output.Get(), Write, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, l->target.Get(), Read, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src {}, dst {};
    src.pResource = s.output.Get();
    dst.pResource = l->target.Get();
    src.Type = dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box { 0, 0, 0, s.width, s.height, 1 };
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    Barrier(cmd, s.output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, Read);
    Barrier(cmd, l->target.Get(), D3D12_RESOURCE_STATE_COPY_DEST, targetState);
    return true;
}
} // namespace DlssNr::PostSr
