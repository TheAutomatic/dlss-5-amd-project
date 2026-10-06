#pragma once
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include "native_lab_paths.h"
#include "native_format_fallback.h"
#include "LmxxfShaderCompiler.h"

namespace LmxxfRuntime
{
// One immutable descriptor pair and every object referenced by the meter dispatch.
// Keep this lease until the recording is invalidated AND all executions complete.
struct ExposureRecording
{
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource* colour = nullptr;
    ID3D12Resource* value = nullptr;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ExposureRecording() = default;
    ExposureRecording(const ExposureRecording&) = delete;
    ExposureRecording& operator=(const ExposureRecording&) = delete;
    ~ExposureRecording()
    {
        for (IUnknown* p : {static_cast<IUnknown*>(heap), static_cast<IUnknown*>(colour),
                           static_cast<IUnknown*>(value), static_cast<IUnknown*>(root),
                           static_cast<IUnknown*>(pso)})
            if (p) p->Release();
    }
};

struct ExposureMeter
{
    static constexpr UINT kSrvRing = 16;
    ID3D12Resource *value = nullptr; // R32_FLOAT 1x1, left in NON_PIXEL_SHADER_RESOURCE
    ID3D12DescriptorHeap *heap = nullptr; // [0] value UAV, [1..kSrvRing] colour SRVs
    ID3D12RootSignature *root = nullptr;
    ID3D12PipelineState *pso = nullptr;
    UINT increment = 0;
    uint64_t frames = 0;
    bool failed = false;

    bool Ready() const { return value && heap && root && pso; }

    // Returns false (and stays failed) if any piece cannot be created; the frame then runs
    // without exposure, as before this existed.
    bool Ensure(ID3D12Device *device)
    {
        if (Ready())
            return true;
        if (failed || !device)
            return false;
        static const char kSource[] = R"(
Texture2D<float4> Colour : register(t0);
RWTexture2D<float> Exposure : register(u0);
cbuffer Region : register(b0) { uint2 Origin; uint2 Extent; };
groupshared float s_sum[256];
groupshared uint s_count[256];
[numthreads(16, 16, 1)]
void main(uint3 t : SV_GroupThreadID, uint i : SV_GroupIndex)
{
    uint2 extent = max(Extent, uint2(1, 1));
    uint2 p = Origin + min(uint2((float2(t.xy) + 0.5) * float2(extent) / 16.0), extent - 1);
    float y = dot(max(Colour.Load(int3(p, 0)).rgb, 0.0), float3(0.2126, 0.7152, 0.0722));
    bool ok = !isnan(y) && !isinf(y);
    s_sum[i] = ok ? y : 0.0;
    s_count[i] = ok ? 1u : 0u;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 128; s > 0; s >>= 1)
    {
        if (i < s) { s_sum[i] += s_sum[i + s]; s_count[i] += s_count[i + s]; }
        GroupMemoryBarrierWithGroupSync();
    }
    if (i != 0 || s_count[0] == 0)
        return; // nothing measurable: keep the previous exposure
    float mean = max(s_sum[0] / float(s_count[0]), 1e-4);
    float encoded = pow(0.45, 2.2);
    float target = clamp((encoded / (1.0 - encoded)) / mean, 1e-4, 100.0);
    float prev = Exposure[uint2(0, 0)];
    bool warm = prev > 0.0 && !isnan(prev) && !isinf(prev);
    Exposure[uint2(0, 0)] = warm ? exp(lerp(log(prev), log(target), 0.25)) : target;
}
)";
        ID3DBlob *code = nullptr, *errors = nullptr;
        HRESULT hr = LmxxfShader::NativeCompileShaderBlob(kSource, sizeof kSource - 1, "lmxxf-exposure-meter", nullptr,
                                             nullptr, "main", &code, &errors, "cs_5_0");
        if (errors)
            errors->Release();
        if (FAILED(hr) || !code)
            return Fail();

        D3D12_DESCRIPTOR_RANGE ranges[2] {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[0].NumDescriptors = 1;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        D3D12_ROOT_PARAMETER params[3] {};
        for (int k = 0; k < 2; ++k)
        {
            params[k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[k].DescriptorTable = {1, &ranges[k]};
            params[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[2].Constants = {0, 0, 4};
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ID3DBlob *serialized = nullptr;
        hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
        if (errors)
            errors->Release();
        if (SUCCEEDED(hr))
            hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                             IID_PPV_ARGS(&root));
        if (serialized)
            serialized->Release();
        if (SUCCEEDED(hr))
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
            pd.pRootSignature = root;
            pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        }
        code->Release();
        if (FAILED(hr))
            return Fail();

        D3D12_DESCRIPTOR_HEAP_DESC hd {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 1 + kSrvRing;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))))
            return Fail();
        increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Committed resources are zero-initialised, and the shader treats 0 as "no history",
        // so the first metered frame takes its target directly.
        D3D12_HEAP_PROPERTIES hp {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = rd.Height = 1;
        rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R32_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                   IID_PPV_ARGS(&value))))
            return Fail();
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud {};
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(value, nullptr, &ud, heap->GetCPUDescriptorHandleForHeapStart());
        return true;
    }

    // colourState is the colour's state at RecordInputs; it is restored before returning.
    void Record(ID3D12GraphicsCommandList *list, ID3D12Device *device, ID3D12Resource *colour,
                D3D12_RESOURCE_STATES colourState, UINT width, UINT height,
                std::shared_ptr<ExposureRecording>* recording = nullptr)
    {
        // Legacy callers retain their serial-frame contract. Recording-lease callers
        // get a private immutable pair; no number of later Record calls can overwrite it.
        if (recording && *recording)
            throw std::runtime_error("ExposureMeter: recording lease already occupied");
        UINT slot = 1;
        ID3D12DescriptorHeap* dispatchHeap = heap;
        std::shared_ptr<ExposureRecording> lease;
        if (recording)
        {
            lease = std::make_shared<ExposureRecording>();
            D3D12_DESCRIPTOR_HEAP_DESC hd {};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = 2;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&lease->heap))))
                throw std::runtime_error("ExposureMeter: recording heap creation failed");
            lease->colour = colour; colour->AddRef();
            lease->value = value; value->AddRef();
            lease->root = root; root->AddRef();
            lease->pso = pso; pso->AddRef();
            dispatchHeap = lease->heap;
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud {};
            ud.Format = DXGI_FORMAT_R32_FLOAT;
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(value, nullptr, &ud,
                                               dispatchHeap->GetCPUDescriptorHandleForHeapStart());
        }
        else slot += static_cast<UINT>(frames++ % kSrvRing);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = dispatchHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(slot) * increment;
        const D3D12_RESOURCE_DESC cd = colour->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd {};
        sd.Format = NativeIsGameColor(cd.Format) ? NativeViewFormat(cd.Format) :
            NativeFallbackColorView(cd.Format) != DXGI_FORMAT_UNKNOWN ? NativeFallbackColorView(cd.Format) :
            NativeViewFormat(cd.Format);
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(colour, &sd, cpu);

        const bool moveColour = (colourState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0;
        D3D12_RESOURCE_BARRIER b[2] {};
        b[0].Type = b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[0].Transition = {value, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        b[1].Transition = {colour, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, colourState,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        list->ResourceBarrier(moveColour ? 2u : 1u, b);

        D3D12_GPU_DESCRIPTOR_HANDLE gpu = dispatchHeap->GetGPUDescriptorHandleForHeapStart();
        list->SetDescriptorHeaps(1, &dispatchHeap);
        list->SetComputeRootSignature(root);
        list->SetPipelineState(pso);
        list->SetComputeRootDescriptorTable(0, gpu);
        gpu.ptr += UINT64(slot) * increment;
        list->SetComputeRootDescriptorTable(1, gpu);
        // Origin is always (0,0): AmdBridge rejects nonzero DLSS colour subrect bases, and
        // job width/height is that top-left subrect (fallback: the whole allocation).
        const UINT region[4] = {0, 0, (std::max)(width, 1u), (std::max)(height, 1u)};
        list->SetComputeRoot32BitConstants(2, 4, region, 0);
        list->Dispatch(1, 1, 1);

        D3D12_RESOURCE_BARRIER a[3] {};
        a[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        a[0].UAV.pResource = value;
        a[1] = b[0];
        a[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        a[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        a[2] = b[1];
        a[2].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        a[2].Transition.StateAfter = colourState;
        list->ResourceBarrier(moveColour ? 3u : 2u, a);
        if (recording) *recording = std::move(lease);
    }

    void Release()
    {
        for (IUnknown *p : {static_cast<IUnknown *>(value), static_cast<IUnknown *>(heap),
                            static_cast<IUnknown *>(root), static_cast<IUnknown *>(pso)})
            if (p)
                p->Release();
        value = nullptr;
        heap = nullptr;
        root = nullptr;
        pso = nullptr;
    }

    // Fail-closed teardown: the GPU may still reference these, so drop them without Release.
    void Abandon()
    {
        value = nullptr;
        heap = nullptr;
        root = nullptr;
        pso = nullptr;
    }

  private:
    bool Fail()
    {
        Release();
        failed = true;
        return false;
    }
};

} // namespace LmxxfRuntime
