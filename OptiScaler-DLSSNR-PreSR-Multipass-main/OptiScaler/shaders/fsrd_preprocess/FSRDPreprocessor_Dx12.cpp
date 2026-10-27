#include "pch.h"
#include "FSRDPreprocessor_Dx12.h"
#include "FSRDShaderUtils.h"
#include "FSRDShaderData.h"
#include "precompile/FSRDInputConv_Shader.h"
#include "precompile/FSRDFloorSeed_Shader.h"
#include "precompile/FSRDFloor_Shader.h"
#include "precompile/FSRDOutputComp_Shader.h"

#include "dx12/ffx_api_dx12.h"
#include "fsr-rr/ffx_denoiser.h"

#include <d3dcompiler.h>
#include <d3d12.h>
#include <stdexcept>
#include <vector>
#include <string>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace FSRD;

constexpr UINT kBackBufferCount = 3;

constexpr UINT kThreadGroupSizeX = 8;
constexpr UINT kThreadGroupSizeY = 8;

constexpr D3D12_RESOURCE_STATES kSrvState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kUavState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

namespace FSRDFormats
{
constexpr DXGI_FORMAT IndirectSpecular = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT DirectDiffuse = DXGI_FORMAT_R16G16B16A16_FLOAT;

// ffxDispatchDescDenoiser
constexpr DXGI_FORMAT Motion = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT Normals = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr DXGI_FORMAT SpecAlbedo = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT DiffAlbedo = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT LinearDepth = DXGI_FORMAT_R32_FLOAT;

constexpr DXGI_FORMAT SkipSignal = DXGI_FORMAT_R16G16B16A16_FLOAT;

// Carries a finished HDR image plus its mix weight, so it needs the same range as
// the skip signal it is an alternative to.
constexpr DXGI_FORMAT Handover = SkipSignal;

constexpr DXGI_FORMAT OutputBuffer1 = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT OutputBuffer2 = DXGI_FORMAT_R16G16B16A16_FLOAT;

constexpr DXGI_FORMAT AmbientOcclusion = DXGI_FORMAT_R8_UNORM;
constexpr DXGI_FORMAT SpecularOcclusion = DXGI_FORMAT_R8_UNORM;
constexpr DXGI_FORMAT DebugView = DXGI_FORMAT_R16G16B16A16_FLOAT;
} // namespace FSRDFormats

// The conversion shader pre-quantizes its albedo outputs to the storage format's levels,
// because the divisor it demodulates with, the value its residual closure accounts for and
// the texel composition remodulates from all have to be the same number - quantizing after
// the arithmetic is what let an albedo of 0.01 store as 3/255 and return 17.6% more light
// than the residual had budgeted. The pairing is cross-language (s_AlbedoStoreLevels in
// FSRDInputConv.hlsl), so a format that stops being the 8-bit UNORM the shader quantizes for
// fails the build here instead of quietly reintroducing that gap.
static_assert(FSRDFormats::SpecAlbedo == DXGI_FORMAT_R8G8B8A8_UNORM,
              "FSRDInputConv quantizes specular albedo to 8-bit UNORM levels");
static_assert(FSRDFormats::DiffAlbedo == DXGI_FORMAT_R8G8B8A8_UNORM,
              "FSRDInputConv quantizes diffuse albedo to 8-bit UNORM levels");

struct ComputeState
{
    ID3D12Device* m_pDev = nullptr;

    ComPtr<ID3D12RootSignature> m_rootSig;
    ComPtr<ID3D12PipelineState> m_pso;
    std::vector<FrameDescriptorHeap> m_frameHeaps;

    ComPtr<ID3D12Resource> m_constUploadBuffer;
    byte* m_cbMappedData = nullptr;
    UINT m_cbSlotSize = 0;
    UINT m_cbCurrentFrameIndex = 0;
    UINT backBufferCount = kBackBufferCount;

    ~ComputeState()
    {
        if (m_constUploadBuffer && m_cbMappedData)
        {
            m_constUploadBuffer->Unmap(0, nullptr);
            m_cbMappedData = nullptr;
        }
    }

    void Initialize(ID3D12Device* pDev, std::span<const byte> bytecode, UINT cbDataSize, UINT numSrvs, UINT numUavs,
                    LPCWSTR cbName, UINT backBufferCount = kBackBufferCount)
    {
        m_pDev = pDev;
        this->backBufferCount = backBufferCount;

        // Create Root Signature
        ThrowIfFailed(m_pDev->CreateRootSignature(0, bytecode.data(), bytecode.size(), IID_PPV_ARGS(&m_rootSig)),
                      "Failed to create Root Signature");

        // Create PSO
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = m_rootSig.Get();
        psoDesc.CS = { bytecode.data(), bytecode.size() };
        ThrowIfFailed(m_pDev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)), "Failed to create PSO");

        // Create Constant Buffer Upload Heap
        m_cbSlotSize = AlignTo256(cbDataSize);
        const UINT bufferSize = m_cbSlotSize * backBufferCount;

        D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = bufferSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ThrowIfFailed(m_pDev->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(&m_constUploadBuffer)),
                      "Failed to create Constant Buffer");

        m_constUploadBuffer->SetName(cbName);
        D3D12_RANGE readRange = { 0, 0 };
        ThrowIfFailed(m_constUploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_cbMappedData)),
                      "Failed to map Constant Buffer");

        m_frameHeaps.resize(backBufferCount);

        // Create Descriptor Heaps
        for (auto& heap : m_frameHeaps)
        {
            if (!heap.Initialize(m_pDev, numSrvs, numUavs, 0, 0))
                throw std::runtime_error("Failed to initialize FrameDescriptorHeap");
        }
    }

    void Dispatch(ID3D12GraphicsCommandList* cmdList, std::span<const byte> cbData,
                  std::span<ID3D12Resource* const> inputs, std::span<const MipChainDesc> inputMips,
                  std::span<ID3D12Resource*> output, std::span<const UINT> outputMips, XMFLOAT2 outDim,
                  bool autoBarrierOutput = true)
    {
        if (!cmdList)
            return;

        ScopedSkipHeapCapture skipHeapCapture {};

        // Constant Buffer Updates
        const UINT currentFrame = m_cbCurrentFrameIndex;
        const UINT currentOffset = currentFrame * m_cbSlotSize;
        memcpy(m_cbMappedData + currentOffset, cbData.data(), cbData.size());

        D3D12_GPU_VIRTUAL_ADDRESS cbAddress = m_constUploadBuffer->GetGPUVirtualAddress() + currentOffset;
        m_cbCurrentFrameIndex = (m_cbCurrentFrameIndex + 1) % backBufferCount;

        // Transitions SRV -> UAV
        if (autoBarrierOutput)
            AddBarriers(cmdList, output, outputMips, kSrvState, kUavState);

        // Update descriptors
        FrameDescriptorHeap& currentHeap = m_frameHeaps[currentFrame];
        CreateSRVs(m_pDev, currentHeap, inputs, inputMips);
        CreateUAVs(m_pDev, currentHeap, output, outputMips);

        // Configure pipeline
        cmdList->SetPipelineState(m_pso.Get());
        cmdList->SetComputeRootSignature(m_rootSig.Get());

        ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootConstantBufferView(0, cbAddress);

        // SRV table
        cmdList->SetComputeRootDescriptorTable(1, currentHeap.GetTableGPUStart());

        // UAV table
        CD3DX12_GPU_DESCRIPTOR_HANDLE uavTable = currentHeap.GetTableGPUStart();
        uavTable.Offset((UINT) inputs.size(),
                        m_pDev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
        cmdList->SetComputeRootDescriptorTable(2, uavTable);

        // Dispatch
        const UINT dimX = ((UINT) outDim.x + (kThreadGroupSizeX - 1)) / kThreadGroupSizeX;
        const UINT dimY = ((UINT) outDim.y + (kThreadGroupSizeY - 1)) / kThreadGroupSizeY;
        cmdList->Dispatch(dimX, dimY, 1);

        // Transition the UAVs back to SRV
        if (autoBarrierOutput)
            AddBarriers(cmdList, output, outputMips, kUavState, kSrvState);
    }

    void Dispatch(ID3D12GraphicsCommandList* cmdList, std::span<const byte> cbData,
                  std::span<ID3D12Resource* const> inputs, std::span<ID3D12Resource*> output, XMFLOAT2 outDim,
                  bool autoBarrierOutput = true)
    {
        Dispatch(cmdList, cbData, inputs, {}, output, {}, outDim, autoBarrierOutput);
    }
};

// Private implementation
struct FSRDPreprocessor_Dx12::Impl
{
    ID3D12Device* m_pDev = nullptr;

    ComputeState m_floorSeedShader;
    ComputeState m_floorFilterShader;
    ComputeState m_convShader;
    ComputeState m_compShader;

    UINT m_maxWidth = 0;
    UINT m_maxHeight = 0;

    // Drives the a-trous filter's temporal pattern rotation. Per instance, not a
    // function-local static: the feature can destroy and recreate the converter, and
    // two live converters would otherwise advance one shared counter twice a frame.
    uint32_t m_floorFilterFrameIndex = 0;

    // Output Targets
    // Internal storage
    Conversion::Output m_out;
    ComPtr<ID3D12Resource> m_LinearDepth;
    ComPtr<ID3D12Resource> m_outputBuffer1;
    ComPtr<ID3D12Resource> m_outputBuffer2;

    ComPtr<ID3D12Resource> m_ambientOcclusionOutput;
    ComPtr<ID3D12Resource> m_specularOcclusionOutput;
    ComPtr<ID3D12Resource> m_debugViewOutput;
    // A replaced debug target cannot be freed while an earlier command list can still
    // reference it. Retain one full descriptor/constant-buffer rotation rather than
    // assuming resolution changes are separated by several frames: DRS may resize on
    // consecutive frames.
    std::array<ComPtr<ID3D12Resource>, kBackBufferCount> m_retiredDebugViewOutputs;
    UINT m_retiredDebugViewOutputIndex = 0;
    UINT m_debugViewWidth = 0;
    UINT m_debugViewHeight = 0;

    // Floor filter
    ID3D12Resource* m_smoothFloor;

    // The RR-facing linear depth for this frame, and the state it was validated in. When
    // a title-published linear depth is being consumed it is that resource - every
    // reconstructed position and the denoiser's own depth input must be the same field.
    ID3D12Resource* m_rrLinearDepth = nullptr;
    uint32_t m_rrLinearDepthState = 0;
    uint32_t m_rrLinearDepthDeclaredState = 0;
    bool m_rrLinearDepthForwarded = false;

    bool m_radianceOutputsInUavState = false;
    bool m_ambientOcclusionOutputInUavState = false;
    bool m_specularOcclusionOutputInUavState = false;
    bool m_debugViewOutputInUavState = false;

    // Diagnostic readback of the three RR-facing g-buffer textures. Presence and
    // the debug views cannot answer whether their *values* are usable: every depth
    // debug view normalises with abs() and a turbo ramp, so a collapsed or
    // mirrored depth field renders as a believable image. Recording happens in
    // DispatchConversion, immediately after the packing dispatch - see
    // RecordInputProbe for why that instant is the only safe one.
    static constexpr UINT kInputProbeTargetCount = 7;
    static constexpr UINT kInputProbeLogDelay = 3;  // minimum conversions between record and read
    static constexpr UINT kInputProbeInterval = 60; // conversions between recordings
    ComPtr<ID3D12Resource> m_inputProbeReadback[kInputProbeTargetCount];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_inputProbeFootprint[kInputProbeTargetCount] = {};
    UINT m_inputProbeRowPitch[kInputProbeTargetCount] = {};
    XMFLOAT4 m_inputProbeMotionTransform = {};
    UINT m_inputProbeWidth = 0;
    UINT m_inputProbeHeight = 0;
    UINT m_inputProbeCountdown = 1; // record on the first conversion, then every interval
    UINT m_inputProbePendingLog = 0;

    // Map synchronises with nothing - Microsoft's readback guidance waits on the fence
    // that follows the submission instead. The copies here ride the title's command
    // list on the title's queue, a queue this code never sees, so there is no fence to
    // wait on. The completion gate is therefore written by the GPU itself: each
    // capture ends with a copy of its generation number into a readback slot, and the
    // data is read only once that token has landed. Everything the readback holds
    // before the token is whatever an earlier submission left behind.
    ComPtr<ID3D12Resource> m_inputProbeGenerationReadback;
    ComPtr<ID3D12Resource> m_inputProbeGenerationUpload;
    void* m_inputProbeGenerationUploadPtr = nullptr;
    UINT64 m_inputProbeGeneration = 0;

    void UpdateInputProbe(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc)
    {
        // Off unless asked for. The probe copies seven render targets into readback buffers and
        // logs six lines every kInputProbeInterval conversions; that is worth its cost while a
        // number is being chased and worth nothing at all in a shipping configuration, where it
        // also wrote a gigabyte of log in a session.
        if (!desc.DiagnosticsEnabled)
            return;

        // A readback heap may be mapped at any time, so polling costs nothing - never
        // a stall or a device fault. The conversion delay is only a floor: the
        // generation token copied after the data is what proves the submission has
        // executed, so a late or batched title submission makes the log retry on
        // later conversions instead of publishing stale or half-written bytes.
        if (m_inputProbePendingLog > 0 && --m_inputProbePendingLog == 0)
        {
            if (!LogInputProbe())
                m_inputProbePendingLog = 1;
        }

        // The readback buffers hold the pending capture until it has been logged;
        // recording again would overwrite bytes that are still owed a read.
        if (m_inputProbePendingLog > 0)
            return;

        if (m_inputProbeCountdown > 0 && --m_inputProbeCountdown > 0)
            return;

        m_inputProbeWidth = static_cast<UINT>(desc.RenderSize.x);
        m_inputProbeHeight = static_cast<UINT>(desc.RenderSize.y);
        m_inputProbeMotionTransform = desc.MotionTransform;
        if (RecordInputProbe(cmdList))
        {
            m_inputProbePendingLog = kInputProbeLogDelay;
            m_inputProbeCountdown = kInputProbeInterval;
        }
    }

    // Copies linear depth, motion vectors and normals into readback buffers.
    //
    // Called from DispatchConversion right after DispatchPackingShader, whose
    // autoBarrierOutput has just returned every output to kSrvState - a state this
    // code owns and can therefore barrier away from. The same three resources must
    // NOT be probed after the denoiser dispatch: the FFX backend records its own
    // barriers over them, and transitioning from an assumed state there removed
    // the device (GPUCrashReport 0xCCCF0D).
    bool RecordInputProbe(ID3D12GraphicsCommandList* cmdList)
    {
        if (m_pDev == nullptr)
            return false;

        ID3D12Resource* sources[kInputProbeTargetCount] = { m_LinearDepth.Get(),
                                                            m_out.Resources.Motion.Get(),
                                                            m_out.Resources.Normals.Get(),
                                                            m_out.Resources.Signals.IndirectSpecular.Get(),
                                                            m_out.Resources.SpecAlbedo.Get(),
                                                            m_out.Resources.SkipSignal.Get(),
                                                            m_out.Resources.DiffAlbedo.Get() };

        for (UINT i = 0; i < kInputProbeTargetCount; i++)
        {
            if (sources[i] == nullptr)
                return false;
        }

        UINT64 maxBufferBytes = 0;
        for (UINT i = 0; i < kInputProbeTargetCount; i++)
        {
            const D3D12_RESOURCE_DESC srcDesc = sources[i]->GetDesc();
            UINT64 rowBytes = 0;
            UINT64 bufferBytes = 0;
            m_pDev->GetCopyableFootprints(&srcDesc, 0, 1, 0, &m_inputProbeFootprint[i], nullptr, &rowBytes,
                                          &bufferBytes);
            if (bufferBytes == 0)
                return false;

            m_inputProbeRowPitch[i] = m_inputProbeFootprint[i].Footprint.RowPitch;
            maxBufferBytes = std::max(maxBufferBytes, bufferBytes);
        }

        // Every buffer has to be present and large enough, not just the first one: the copy loop
        // below indexes the whole set, so the invariant it depends on is about the whole set.
        // Testing element 0 alone is what let a resize that failed part way through be read as
        // complete - the element that had already been replaced was the right size, the test
        // passed, and the next attempt copied into a null or undersized buffer.
        bool needsReadbackBuffers = false;
        for (UINT i = 0; i < kInputProbeTargetCount; i++)
        {
            if (m_inputProbeReadback[i] == nullptr || m_inputProbeReadback[i]->GetDesc().Width < maxBufferBytes)
            {
                needsReadbackBuffers = true;
                break;
            }
        }

        if (needsReadbackBuffers)
        {
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_READBACK;

            D3D12_RESOURCE_DESC bufDesc = {};
            bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufDesc.Width = maxBufferBytes;
            bufDesc.Height = 1;
            bufDesc.DepthOrArraySize = 1;
            bufDesc.MipLevels = 1;
            bufDesc.SampleDesc = { 1, 0 };
            bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            // Build the whole set before any of it is committed, so the array is never observed
            // holding a mixture of new, null and stale buffers. A partial commit is exactly the
            // state the sizing test above can no longer see, and the copies below cannot
            // survive it. The set being replaced also stays alive until replacements exist.
            std::array<ComPtr<ID3D12Resource>, kInputProbeTargetCount> allocated;
            for (UINT i = 0; i < kInputProbeTargetCount; i++)
            {
                if (FAILED(m_pDev->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                           IID_PPV_ARGS(&allocated[i]))))
                {
                    LOG_ERROR("[RR_INPUT_PROBE] readback buffer creation failed");
                    return false;
                }
            }

            for (UINT i = 0; i < kInputProbeTargetCount; i++)
                m_inputProbeReadback[i] = std::move(allocated[i]);
        }

        // The completion token for the capture: one 64-bit generation, written by the
        // GPU from an upload slot after the data copies below. Created once; the data
        // readbacks above are the only things that grow with resolution.
        if (m_inputProbeGenerationReadback == nullptr)
        {
            D3D12_HEAP_PROPERTIES tokenHeaps[2] = {};
            tokenHeaps[0].Type = D3D12_HEAP_TYPE_READBACK;
            tokenHeaps[1].Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC tokenDesc = {};
            tokenDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            tokenDesc.Width = sizeof(UINT64);
            tokenDesc.Height = 1;
            tokenDesc.DepthOrArraySize = 1;
            tokenDesc.MipLevels = 1;
            tokenDesc.SampleDesc = { 1, 0 };
            tokenDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            if (FAILED(m_pDev->CreateCommittedResource(&tokenHeaps[0], D3D12_HEAP_FLAG_NONE, &tokenDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&m_inputProbeGenerationReadback))) ||
                FAILED(m_pDev->CreateCommittedResource(&tokenHeaps[1], D3D12_HEAP_FLAG_NONE, &tokenDesc,
                                                       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                       IID_PPV_ARGS(&m_inputProbeGenerationUpload))) ||
                FAILED(m_inputProbeGenerationUpload->Map(0, nullptr, &m_inputProbeGenerationUploadPtr)) ||
                m_inputProbeGenerationUploadPtr == nullptr)
            {
                LOG_ERROR("[RR_INPUT_PROBE] generation token buffer creation failed");
                m_inputProbeGenerationReadback.Reset();
                m_inputProbeGenerationUpload.Reset();
                m_inputProbeGenerationUploadPtr = nullptr;
                return false;
            }
        }

        D3D12_RESOURCE_BARRIER toCopy[kInputProbeTargetCount] = {};
        D3D12_RESOURCE_BARRIER toSrv[kInputProbeTargetCount] = {};
        for (UINT i = 0; i < kInputProbeTargetCount; i++)
        {
            toCopy[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy[i].Transition.pResource = sources[i];
            toCopy[i].Transition.StateBefore = kSrvState;
            toCopy[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            toCopy[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

            toSrv[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toSrv[i].Transition.pResource = sources[i];
            toSrv[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            toSrv[i].Transition.StateAfter = kSrvState;
            toSrv[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }

        cmdList->ResourceBarrier(kInputProbeTargetCount, toCopy);
        for (UINT i = 0; i < kInputProbeTargetCount; i++)
        {
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = m_inputProbeReadback[i].Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = m_inputProbeFootprint[i];

            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = sources[i];
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;

            cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        cmdList->ResourceBarrier(kInputProbeTargetCount, toSrv);

        // Recorded after every data copy: the GPU executes command list operations in
        // order, so the token's arrival in readback is the completion proof for the
        // whole batch above. The upload slot is safe to overwrite because recording
        // only happens once the previous capture's token has been observed, which
        // means its submission has already executed and consumed the old value.
        ++m_inputProbeGeneration;
        memcpy(m_inputProbeGenerationUploadPtr, &m_inputProbeGeneration, sizeof(m_inputProbeGeneration));
        cmdList->CopyBufferRegion(m_inputProbeGenerationReadback.Get(), 0, m_inputProbeGenerationUpload.Get(), 0,
                                  sizeof(m_inputProbeGeneration));

        return true;
    }

    // IEEE 754 half -> float for the readbacks (this translation unit has no
    // DirectXPackedVector, and the probe must not depend on one).
    static float ProbeHalfToFloat(uint16_t h)
    {
        const uint32_t sign = (h >> 15) & 1u;
        uint32_t exponent = (h >> 10) & 0x1Fu;
        uint32_t mantissa = h & 0x3FFu;

        uint32_t bits = 0;
        if (exponent == 0u)
        {
            if (mantissa != 0u)
            {
                exponent = 1u;
                while ((mantissa & 0x400u) == 0u)
                {
                    mantissa <<= 1;
                    exponent--;
                }
                mantissa &= 0x3FFu;
                bits = (sign << 31) | ((exponent - 15u + 127u) << 23) | (mantissa << 13);
            }
            else
                bits = sign << 31;
        }
        else if (exponent == 0x1Fu)
            bits = (sign << 31) | 0x7F800000u | (mantissa << 13);
        else
            bits = (sign << 31) | ((exponent - 15u + 127u) << 23) | (mantissa << 13);

        float result = 0.0f;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    static float PercentileOf(std::vector<float>& sorted, float fraction)
    {
        if (sorted.empty())
            return 0.0f;

        const size_t index = static_cast<size_t>(fraction * float(sorted.size() - 1));
        return sorted[index];
    }

    struct ProbeSampling
    {
        UINT strideX = 1;
        UINT strideY = 1;
        UINT samples = 0;
    };

    static ProbeSampling GetProbeSampling(UINT width, UINT height)
    {
        ProbeSampling sampling;
        sampling.strideX = std::max(width / 128u, 1u);
        sampling.strideY = std::max(height / 72u, 1u);
        sampling.samples =
            ((width + sampling.strideX - 1) / sampling.strideX) * ((height + sampling.strideY - 1) / sampling.strideY);
        return sampling;
    }

    // Reports the RR-facing view depth. The question this answers is scale, not
    // sign: a world reconstructed at a fraction of a metre and one at hundreds of
    // metres are the same picture in every normalised debug view, but only the
    // second can drive RR's reprojection.
    void LogLinearDepthProbe(const uint8_t* data, UINT rowPitch, const ProbeSampling& sampling)
    {
        const UINT sampleWidth = std::min(m_inputProbeWidth, m_maxWidth);
        const UINT sampleHeight = std::min(m_inputProbeHeight, m_maxHeight);

        std::vector<float> magnitudes;
        magnitudes.reserve(sampling.samples);

        float minValue = 0.0f;
        float maxValue = 0.0f;
        bool first = true;
        UINT negatives = 0;
        UINT zeros = 0;
        UINT nonFinite = 0;
        double sum = 0.0;
        UINT buckets[6] = {}; // |v| < 0.1, < 1, < 10, < 100, < 1000, >= 1000

        for (UINT y = sampling.strideY / 2; y < sampleHeight; y += sampling.strideY)
        {
            const float* row = reinterpret_cast<const float*>(data + size_t(y) * rowPitch);
            for (UINT x = sampling.strideX / 2; x < sampleWidth; x += sampling.strideX)
            {
                const float value = row[x];
                if (!std::isfinite(value))
                {
                    ++nonFinite;
                    continue;
                }

                const float magnitude = std::abs(value);
                magnitudes.push_back(magnitude);
                sum += double(magnitude);

                if (value < 0.0f)
                    ++negatives;
                if (value == 0.0f)
                    ++zeros;

                if (first)
                {
                    minValue = value;
                    maxValue = value;
                    first = false;
                }
                else
                {
                    minValue = std::min(minValue, value);
                    maxValue = std::max(maxValue, value);
                }

                const UINT bucket = magnitude < 0.1f      ? 0u
                                    : magnitude < 1.0f    ? 1u
                                    : magnitude < 10.0f   ? 2u
                                    : magnitude < 100.0f  ? 3u
                                    : magnitude < 1000.0f ? 4u
                                                          : 5u;
                ++buckets[bucket];
            }
        }

        if (magnitudes.empty())
        {
            LOG_WARN("[RR_INPUT_PROBE] linearDepth: no finite samples");
            return;
        }

        std::sort(magnitudes.begin(), magnitudes.end());
        const float rcpCount = 100.0f / float(magnitudes.size());

        LOG_INFO("[RR_INPUT_PROBE] linearDepth (R32_FLOAT, RR-facing) {}x{}: samples={}, min={:.4f}, max={:.4f}, "
                 "mean|v|={:.4f}, p50|v|={:.4f}, p95|v|={:.4f}, negative={:.1f}%, exactZero={:.1f}%, nonFinite={}",
                 sampleWidth, sampleHeight, UINT(magnitudes.size()), minValue, maxValue,
                 sum / double(magnitudes.size()), PercentileOf(magnitudes, 0.50f), PercentileOf(magnitudes, 0.95f),
                 float(negatives) * rcpCount, float(zeros) * rcpCount, nonFinite);

        const char* bucketNames[6] = { "<0.1 (sub-metre)", "0.1-1", "1-10", "10-100", "100-1000", ">=1000" };
        LOG_INFO("[RR_INPUT_PROBE] linearDepth |v| distribution: {}={:.1f}%, {}={:.1f}%, {}={:.1f}%, "
                 "{}={:.1f}%, {}={:.1f}%, {}={:.1f}%",
                 bucketNames[0], float(buckets[0]) * rcpCount, bucketNames[1], float(buckets[1]) * rcpCount,
                 bucketNames[2], float(buckets[2]) * rcpCount, bucketNames[3], float(buckets[3]) * rcpCount,
                 bucketNames[4], float(buckets[4]) * rcpCount, bucketNames[5], float(buckets[5]) * rcpCount);
    }

    // Reports the canonical motion field in pixels, the range actually stored in the
    // source texture, and its depth delta.
    //
    // Two questions need separate answers here. Whether the field is alive at all is
    // read from the pixel magnitudes: a moving camera produces tens of pixels, a
    // frozen scene produces exact zeros. Whether the stored values are pixel- or
    // UV-space is read by dividing the canonical value back out by the transform the
    // converter applied - the number that comes back is what the title wrote, and
    // its magnitude says which convention it used.
    void LogMotionProbe(const uint8_t* data, UINT rowPitch, const ProbeSampling& sampling)
    {
        const UINT sampleWidth = std::min(m_inputProbeWidth, m_maxWidth);
        const UINT sampleHeight = std::min(m_inputProbeHeight, m_maxHeight);

        std::vector<float> pixelMagnitudes;
        std::vector<float> depthDeltas;
        std::vector<float> storedMagnitudes;
        pixelMagnitudes.reserve(sampling.samples);
        depthDeltas.reserve(sampling.samples);
        storedMagnitudes.reserve(sampling.samples);

        UINT nonFinite = 0;
        UINT zeroXy = 0;
        UINT activeXy = 0;

        const float transformX = m_inputProbeMotionTransform.x;
        const float transformY = m_inputProbeMotionTransform.y;
        const bool transformInvertible =
            std::isfinite(transformX) && std::isfinite(transformY) && transformX != 0.0f && transformY != 0.0f;

        for (UINT y = sampling.strideY / 2; y < sampleHeight; y += sampling.strideY)
        {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(data + size_t(y) * rowPitch);
            for (UINT x = sampling.strideX / 2; x < sampleWidth; x += sampling.strideX)
            {
                const float motionX = ProbeHalfToFloat(row[x * 4 + 0]);
                const float motionY = ProbeHalfToFloat(row[x * 4 + 1]);
                const float depthDelta = ProbeHalfToFloat(row[x * 4 + 2]);

                if (!std::isfinite(motionX) || !std::isfinite(motionY) || !std::isfinite(depthDelta))
                {
                    ++nonFinite;
                    continue;
                }

                // Canonical motion is a UV displacement, so pixels need the render extent.
                const float pixelsX = motionX * float(sampleWidth);
                const float pixelsY = motionY * float(sampleHeight);
                const float magnitude = std::sqrt(pixelsX * pixelsX + pixelsY * pixelsY);
                pixelMagnitudes.push_back(magnitude);
                depthDeltas.push_back(std::abs(depthDelta));

                if (transformInvertible)
                {
                    const float storedX = motionX / transformX;
                    const float storedY = motionY / transformY;
                    storedMagnitudes.push_back(std::sqrt(storedX * storedX + storedY * storedY));
                }

                if (motionX == 0.0f && motionY == 0.0f)
                    ++zeroXy;
                if (magnitude > 0.05f)
                    ++activeXy;
            }
        }

        if (pixelMagnitudes.empty())
        {
            LOG_WARN("[RR_INPUT_PROBE] motion: no finite samples");
            return;
        }

        std::sort(pixelMagnitudes.begin(), pixelMagnitudes.end());
        std::sort(depthDeltas.begin(), depthDeltas.end());
        std::sort(storedMagnitudes.begin(), storedMagnitudes.end());
        const float rcpCount = 100.0f / float(pixelMagnitudes.size());

        LOG_INFO("[RR_INPUT_PROBE] motion (RGBA16F, RR-facing) {}x{}: samples={}, canonicalPixels p50={:.3f}, "
                 "p95={:.3f}, max={:.3f}, exactZeroXY={:.1f}%, moving(>0.05px)={:.1f}%, |depthDelta| p50={:.6f}, "
                 "p95={:.6f}, max={:.6f}, nonFinite={}",
                 sampleWidth, sampleHeight, UINT(pixelMagnitudes.size()), PercentileOf(pixelMagnitudes, 0.50f),
                 PercentileOf(pixelMagnitudes, 0.95f), pixelMagnitudes.back(), float(zeroXy) * rcpCount,
                 float(activeXy) * rcpCount, PercentileOf(depthDeltas, 0.50f), PercentileOf(depthDeltas, 0.95f),
                 depthDeltas.back(), nonFinite);

        // The converter multiplies the stored value by MotionTransform to reach UV. A
        // transform of 1/renderWidth means the stored value was assumed to be a
        // render-pixel delta; a stored magnitude near 1e-3 with that transform is the
        // signature of a title that supplies UV-space motion instead.
        if (!storedMagnitudes.empty())
        {
            LOG_INFO("[RR_INPUT_PROBE] motion transform=(x={:.8f}, y={:.8f}) => impliedScale=({:.4f}, {:.4f}); "
                     "stored-in-texture|v| p50={:.6f}, p95={:.6f}, max={:.6f}",
                     transformX, transformY, transformX * float(sampleWidth), transformY * float(sampleHeight),
                     PercentileOf(storedMagnitudes, 0.50f), PercentileOf(storedMagnitudes, 0.95f),
                     storedMagnitudes.back());
        }
    }

    // Reports the octahedrally encoded world normals, their packed roughness and the
    // material type. A decoded length far from one means RR's edge weights are being
    // computed from something that is not a direction.
    void LogNormalsProbe(const uint8_t* data, UINT rowPitch, const ProbeSampling& sampling)
    {
        const UINT sampleWidth = std::min(m_inputProbeWidth, m_maxWidth);
        const UINT sampleHeight = std::min(m_inputProbeHeight, m_maxHeight);

        std::vector<float> decodedLengths;
        std::vector<float> upComponents;
        decodedLengths.reserve(sampling.samples);
        upComponents.reserve(sampling.samples);

        UINT degenerate = 0;
        UINT exactZeroRoughness = 0;
        UINT typeZero = 0;
        UINT pointingDown = 0;
        UINT horizontal = 0;
        double roughnessSum = 0.0;

        for (UINT y = sampling.strideY / 2; y < sampleHeight; y += sampling.strideY)
        {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(data + size_t(y) * rowPitch);
            for (UINT x = sampling.strideX / 2; x < sampleWidth; x += sampling.strideX)
            {
                // R10G10B10A2_UNORM: R in bits 0-9, G 10-19, B 20-29, A 30-31.
                const uint32_t packed = row[x];
                const float encodedX = float(packed & 0x3FFu) / 1023.0f;
                const float encodedY = float((packed >> 10) & 0x3FFu) / 1023.0f;
                const float roughness = float((packed >> 20) & 0x3FFu) / 1023.0f;
                const uint32_t materialType = (packed >> 30) & 0x3u;

                // Mirrors OctahedralDecode in FSRDPreprocessCommon.hlsli.
                const float octX = 2.0f * (encodedX - 0.5f);
                const float octY = 2.0f * (encodedY - 0.5f);
                float normalX = octX;
                float normalY = octY;
                const float normalZ = 1.0f - std::abs(octX) - std::abs(octY);
                const float fold = std::max(-normalZ, 0.0f);
                normalX += (normalX >= 0.0f) ? -fold : fold;
                normalY += (normalY >= 0.0f) ? -fold : fold;

                const float length = std::sqrt(normalX * normalX + normalY * normalY + normalZ * normalZ);
                decodedLengths.push_back(length);
                if (length > 1e-6f)
                {
                    const float normalizedY = normalY / length;
                    upComponents.push_back(normalizedY);
                    if (normalizedY < -0.9f)
                        ++pointingDown;
                    else if (std::abs(normalizedY) < 0.1f)
                        ++horizontal;
                }

                if (length < 0.5f)
                    ++degenerate;
                if (roughness == 0.0f)
                    ++exactZeroRoughness;
                if (materialType == 0u)
                    ++typeZero;
                roughnessSum += double(roughness);
            }
        }

        if (decodedLengths.empty())
        {
            LOG_WARN("[RR_INPUT_PROBE] normals: no samples");
            return;
        }

        std::sort(decodedLengths.begin(), decodedLengths.end());
        std::sort(upComponents.begin(), upComponents.end());
        const float rcpCount = 100.0f / float(decodedLengths.size());

        LOG_INFO("[RR_INPUT_PROBE] normals (R10G10B10A2, RR-facing) {}x{}: samples={}, |octDecode| p50={:.4f}, "
                 "p95={:.4f}, max={:.4f}, degenerate(<0.5)={:.1f}%, decodedWorldY p50={:.4f}, p95={:.4f}, "
                 "downward(Y<-0.9)={:.1f}%, horizontal(|Y|<0.1)={:.1f}%",
                 sampleWidth, sampleHeight, UINT(decodedLengths.size()), PercentileOf(decodedLengths, 0.50f),
                 PercentileOf(decodedLengths, 0.95f), decodedLengths.back(), float(degenerate) * rcpCount,
                 PercentileOf(upComponents, 0.50f), PercentileOf(upComponents, 0.95f), float(pointingDown) * rcpCount,
                 float(horizontal) * rcpCount);

        LOG_INFO("[RR_INPUT_PROBE] normals channels: roughness mean={:.4f}, exactZero={:.1f}%, "
                 "materialType 0 (no handover)={:.1f}%, >=1={:.1f}%",
                 float(roughnessSum / double(decodedLengths.size())), float(exactZeroRoughness) * rcpCount,
                 float(typeZero) * rcpCount, float(decodedLengths.size() - typeZero) * rcpCount);
    }

    // Reports the specular signal the converter hands RR: RGB is demodulated radiance,
    // A is the ray length. The diffuse counterpart is probed from the feature side; this
    // one has never been measured, and it is the signal whose alpha carries the
    // hit-distance contract, so a dead or sentinel-only field here would be invisible
    // in every debug view that shows the composed image.
    void LogSignalProbe(const uint8_t* data, UINT rowPitch, const ProbeSampling& sampling)
    {
        const UINT sampleWidth = std::min(m_inputProbeWidth, m_maxWidth);
        const UINT sampleHeight = std::min(m_inputProbeHeight, m_maxHeight);

        UINT samples = 0;
        UINT dead = 0;
        UINT bright = 0;
        UINT nonFinite = 0;
        UINT negativeAlpha = 0;
        UINT missingAlpha = 0; // the FP16-max "ray miss" sentinel
        double lumaSum = 0.0;
        double alphaSum = 0.0;
        float lumaMax = 0.0f;
        float alphaMax = 0.0f;
        float alphaMin = 0.0f;
        float dumped[4][4] = {};

        for (UINT y = sampling.strideY / 2; y < sampleHeight; y += sampling.strideY)
        {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(data + size_t(y) * rowPitch);
            for (UINT x = sampling.strideX / 2; x < sampleWidth; x += sampling.strideX)
            {
                const float r = ProbeHalfToFloat(row[x * 4 + 0]);
                const float g = ProbeHalfToFloat(row[x * 4 + 1]);
                const float b = ProbeHalfToFloat(row[x * 4 + 2]);
                const float a = ProbeHalfToFloat(row[x * 4 + 3]);

                if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a))
                {
                    ++nonFinite;
                    continue;
                }

                const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                ++samples;
                lumaSum += double(luma);
                alphaSum += double(a);
                lumaMax = std::max(lumaMax, luma);
                alphaMax = std::max(alphaMax, a);

                if (luma < 1e-5f)
                    ++dead;
                if (luma > 1e-2f)
                    ++bright;
                if (a < 0.0f)
                    ++negativeAlpha;
                else if (a >= 65504.0f)
                    ++missingAlpha;

                alphaMin = (samples == 1u) ? a : std::min(alphaMin, a);

                if (samples <= 4u)
                {
                    dumped[samples - 1u][0] = luma;
                    dumped[samples - 1u][1] = a;
                }
            }
        }

        // Keep the first four luminance samples in the same record as the statistics so a
        // value-level reading does not need a second pass.
        if (samples == 0)
        {
            LOG_WARN("[RR_INPUT_PROBE] specular signal: no finite samples");
            return;
        }

        const float rcpCount = 100.0f / float(samples);
        LOG_INFO("[RR_INPUT_PROBE] specular signal input (RGBA16F, RR-facing) {}x{}: samples={}, avgLuma={:.6f}, "
                 "maxLuma={:.6f}, dead(<1e-5)={:.1f}%, bright(>1e-2)={:.1f}%, alpha min={:.4f}, mean={:.4f}, "
                 "max={:.4f}, negative={:.1f}%, FP16maxSentinel={:.1f}%, nonFinite={}",
                 sampleWidth, sampleHeight, samples, lumaSum / double(samples), lumaMax, float(dead) * rcpCount,
                 float(bright) * rcpCount, alphaMin, alphaSum / double(samples), alphaMax,
                 float(negativeAlpha) * rcpCount, float(missingAlpha) * rcpCount, nonFinite);
    }

    // Reports how much of the frame reaches the denoiser through the specular signal rather than
    // the diffuse one, read from the share the conversion shader leaves in the specular albedo's
    // unused alpha. It is the title's own material split, so it says how much of the image is
    // exposed to the specular signal's handling - including a ray-length guide the denoiser may
    // refuse to denoise at all, which is a failure mode this probe makes visible as a number.
    void LogSpecularShareProbe(const uint8_t* data, UINT rowPitch, const ProbeSampling& sampling)
    {
        const UINT sampleWidth = std::min(m_inputProbeWidth, m_maxWidth);
        const UINT sampleHeight = std::min(m_inputProbeHeight, m_maxHeight);

        UINT samples = 0;
        UINT specularDominant = 0;
        double shareSum = 0.0;

        for (UINT y = sampling.strideY / 2; y < sampleHeight; y += sampling.strideY)
        {
            const uint8_t* row = data + size_t(y) * rowPitch;
            for (UINT x = sampling.strideX / 2; x < sampleWidth; x += sampling.strideX)
            {
                ++samples;
                const uint32_t share = row[x * 4 + 3];
                shareSum += double(share) / 255.0;
                if (share >= 128u)
                    ++specularDominant;
            }
        }

        if (samples == 0)
            return;

        LOG_INFO("[RR_INPUT_PROBE] specular signal share: mean {:.2f}%, specular-dominant "
                 "{:.2f}% ({}/{}) - the share of the frame the denoiser receives through the "
                 "specular signal",
                 float(shareSum) * 100.0f / float(samples), float(specularDominant) * 100.0f / float(samples),
                 specularDominant, samples);
    }

    // Magnitude of a colour texture, which is all the skip signal needs: the question is how
    // much colour it carries and whether that quantity is what the visible noise tracks.
    void LogMagnitudeProbe(const char* name, const uint8_t* data, UINT rowPitch, const ProbeSampling& sampling,
                           bool halfPrecision)
    {
        const UINT sampleWidth = std::min(m_inputProbeWidth, m_maxWidth);
        const UINT sampleHeight = std::min(m_inputProbeHeight, m_maxHeight);

        UINT samples = 0;
        UINT dead = 0;
        double lumaSum = 0.0;
        float lumaMax = 0.0f;

        for (UINT y = sampling.strideY / 2; y < sampleHeight; y += sampling.strideY)
        {
            const uint8_t* row = data + size_t(y) * rowPitch;
            for (UINT x = sampling.strideX / 2; x < sampleWidth; x += sampling.strideX)
            {
                float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
                if (halfPrecision)
                {
                    const uint16_t* h = reinterpret_cast<const uint16_t*>(row) + size_t(x) * 4;
                    r = ProbeHalfToFloat(h[0]);
                    g = ProbeHalfToFloat(h[1]);
                    b = ProbeHalfToFloat(h[2]);
                    a = ProbeHalfToFloat(h[3]);
                }
                else
                {
                    const uint8_t* p = row + size_t(x) * 4;
                    r = float(p[0]) / 255.0f;
                    g = float(p[1]) / 255.0f;
                    b = float(p[2]) / 255.0f;
                    a = float(p[3]) / 255.0f;
                }

                if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a))
                    continue;

                ++samples;
                const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                lumaSum += double(luma);
                lumaMax = std::max(lumaMax, luma);
                if (luma < 1e-5f)
                    ++dead;
            }
        }

        if (samples == 0)
            return;

        LOG_INFO("[RR_INPUT_PROBE] {}: samples={}, avgLuma={:.6f}, maxLuma={:.6f}, dead(<1e-5)={:.1f}%", name, samples,
                 lumaSum / double(samples), lumaMax, float(dead) * 100.0f / float(samples));
    }

    // Share of the frame whose published floor exceeds its raw sample. The conversion marks
    // those pixels in the alpha channel. On them the residual handed to the denoiser is zero,
    // so the skip signal is the floor filter's own low pass and nothing else - which is both
    // where the softness comes from and the set an energy clamp used to fill with the raw
    // sample. It should fall toward zero as FloorEnvelopeBias rises.
    void LogCrossingShareProbe(const uint8_t* data, UINT rowPitch, const ProbeSampling& sampling)
    {
        const UINT sampleWidth = std::min(m_inputProbeWidth, m_maxWidth);
        const UINT sampleHeight = std::min(m_inputProbeHeight, m_maxHeight);

        UINT samples = 0;
        UINT crossing = 0;

        for (UINT y = sampling.strideY / 2; y < sampleHeight; y += sampling.strideY)
        {
            const uint8_t* row = data + size_t(y) * rowPitch;
            for (UINT x = sampling.strideX / 2; x < sampleWidth; x += sampling.strideX)
            {
                ++samples;
                if (row[size_t(x) * 4 + 3] > 128)
                    ++crossing;
            }
        }

        if (samples == 0)
            return;

        LOG_INFO("[RR_INPUT_PROBE] floor/raw crossing: {:.2f}% of the frame publishes the floor "
                 "over the raw sample (the denoiser is handed nothing there)",
                 float(crossing) * 100.0f / float(samples));
    }

    // Returns false when the capture's copy submission has not executed yet, or the
    // readback could not be mapped this attempt; the caller retries on a later
    // conversion. Returning true consumes the pending log whatever the outcome.
    bool LogInputProbe()
    {
        if (m_pDev == nullptr || m_inputProbeWidth == 0 || m_inputProbeHeight == 0)
            return true;

        // The map loop below indexes the whole set, so it checks the whole set rather than
        // taking element 0 as a proxy for it - the assumption that let a partially rebuilt
        // array through on the allocation side would be a null dereference here.
        for (UINT i = 0; i < kInputProbeTargetCount; i++)
        {
            if (m_inputProbeReadback[i] == nullptr)
                return true;
        }

        if (m_inputProbeGenerationReadback != nullptr)
        {
            void* token = nullptr;
            UINT64 generation = 0;
            if (SUCCEEDED(m_inputProbeGenerationReadback->Map(0, nullptr, &token)) && token != nullptr)
            {
                memcpy(&generation, token, sizeof(generation));
                m_inputProbeGenerationReadback->Unmap(0, nullptr);
            }

            // The token is the GPU's own completion signal for the submission that
            // carried the data copies. Until it matches, everything mapped below
            // would be stale or partially written bytes from an earlier capture.
            if (generation != m_inputProbeGeneration)
                return false;
        }

        void* mapped[kInputProbeTargetCount] = {};
        for (UINT i = 0; i < kInputProbeTargetCount; i++)
        {
            if (FAILED(m_inputProbeReadback[i]->Map(0, nullptr, &mapped[i])) || mapped[i] == nullptr)
            {
                LOG_ERROR("[RR_INPUT_PROBE] readback map failed for target {}", i);
                for (UINT j = 0; j < i; j++)
                    m_inputProbeReadback[j]->Unmap(0, nullptr);
                return false;
            }
        }

        const ProbeSampling sampling = GetProbeSampling(m_inputProbeWidth, m_inputProbeHeight);
        LOG_INFO("[RR_INPUT_PROBE] reading back {}x{} render extent from the conversion", m_inputProbeWidth,
                 m_inputProbeHeight);

        LogLinearDepthProbe(static_cast<const uint8_t*>(mapped[0]), m_inputProbeRowPitch[0], sampling);
        LogMotionProbe(static_cast<const uint8_t*>(mapped[1]), m_inputProbeRowPitch[1], sampling);
        LogNormalsProbe(static_cast<const uint8_t*>(mapped[2]), m_inputProbeRowPitch[2], sampling);
        LogSignalProbe(static_cast<const uint8_t*>(mapped[3]), m_inputProbeRowPitch[3], sampling);
        LogSpecularShareProbe(static_cast<const uint8_t*>(mapped[4]), m_inputProbeRowPitch[4], sampling);
        LogMagnitudeProbe("skip signal (RR-facing)", static_cast<const uint8_t*>(mapped[5]), m_inputProbeRowPitch[5],
                          sampling, true);
        LogCrossingShareProbe(static_cast<const uint8_t*>(mapped[6]), m_inputProbeRowPitch[6], sampling);

        for (UINT i = 0; i < kInputProbeTargetCount; i++)
            m_inputProbeReadback[i]->Unmap(0, nullptr);

        return true;
    }

    void Initialize(std::span<const byte> blSeedByteCode, std::span<const byte> blPyramidByteCode,
                    std::span<const byte> convByteCode, std::span<const byte> compByteCode)
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        LOG_DEBUG("Creating FSRD interop shaders...");

        m_floorSeedShader.Initialize(m_pDev, blSeedByteCode, sizeof(FloorSeed::Constants), FloorSeed::Input::kCount,
                                     FloorSeed::Output::kCount, L"FSRD_FloorSeed_Constants",
                                     FloorSeed::kBackBufferCount);
        m_floorFilterShader.Initialize(m_pDev, blPyramidByteCode, sizeof(FloorFilter::Constants),
                                       FloorFilter::Input::kCount, FloorFilter::Output::kCount,
                                       L"FSRD_FloorFilter_Constants", FloorFilter::kBackBufferCount);
        m_convShader.Initialize(m_pDev, convByteCode, sizeof(Conversion::Constants), Conversion::Input::kCount,
                                Conversion::Output::kCount, L"FSRD_Conv_Constants", Conversion::kBackBufferCount);
        m_compShader.Initialize(m_pDev, compByteCode, sizeof(Composition::Constants), Composition::Input::kCount,
                                Composition::kOutputCount, L"FSRD_Comp_Constants", Composition::kBackBufferCount);

        LOG_DEBUG("FSRD interop shaders and resources initialized.");
    }

    void SetMaxRenderSize(UINT width, UINT height)
    {
        if (m_maxWidth == width && m_maxHeight == height)
            return;

        // Clear the latch before allocating rather than after. CreateTexture2D
        // throws on failure, which leaves the object holding a mix of new- and
        // old-sized textures; if the requested dimensions were already latched, an
        // identical retry would hit the early-out above and silently "succeed" with
        // that mix still in place. The latch is set at the end, once every
        // allocation has actually completed.
        m_maxWidth = 0;
        m_maxHeight = 0;

        auto CreateTex = [&](DXGI_FORMAT fmt, LPCWSTR name, UINT mipLevels = 1)
        { return CreateTexture2D(m_pDev, width, height, fmt, name, kSrvState, mipLevels); };

        auto& outResources = m_out.Resources;
        outResources.Motion = CreateTex(FSRDFormats::Motion, L"FSR_Conv_Motion");
        outResources.Normals = CreateTex(FSRDFormats::Normals, L"FSR_Conv_Normals");
        outResources.SpecAlbedo = CreateTex(FSRDFormats::SpecAlbedo, L"FSR_Conv_SpecAlbedo");
        outResources.DiffAlbedo = CreateTex(FSRDFormats::DiffAlbedo, L"FSR_Conv_DiffAlbedo");
        outResources.SkipSignal = CreateTex(FSRDFormats::SkipSignal, L"FSR_Conv_SkipSignal");
        outResources.Handover = CreateTex(FSRDFormats::Handover, L"FSR_Conv_Handover");
        m_LinearDepth = CreateTex(FSRDFormats::LinearDepth, L"FSR_Conv_LinearDepth");
        m_outputBuffer1 = CreateTex(FSRDFormats::OutputBuffer1, L"FSR_Conv_OutputBuffer1");
        m_outputBuffer2 = CreateTex(FSRDFormats::OutputBuffer2, L"FSR_Conv_OutputBuffer2");
        m_ambientOcclusionOutput = CreateTex(FSRDFormats::AmbientOcclusion, L"FSR_RR_AmbientOcclusion_Output");
        m_specularOcclusionOutput = CreateTex(FSRDFormats::SpecularOcclusion, L"FSR_RR_SpecularOcclusion_Output");

        m_smoothFloor = nullptr;
        m_radianceOutputsInUavState = false;
        m_ambientOcclusionOutputInUavState = false;
        m_specularOcclusionOutputInUavState = false;

        outResources.Signals = { .IndirectSpecular =
                                     CreateTex(FSRDFormats::IndirectSpecular, L"FSR_Conv_IndirectSpecular"),
                                 .DirectDiffuse = CreateTex(FSRDFormats::DirectDiffuse, L"FSR_Conv_DirectDiffuse") };

        // Every allocation succeeded, so the new size is now the real state.
        m_maxWidth = width;
        m_maxHeight = height;
    }

    void DispatchFloorSeed(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc)
    {
        const XMFLOAT2 dispatchSize = { desc.RenderSize.x, desc.RenderSize.y };
        const bool isDepthLinear = (desc.Flags & (uint32_t) ConvFlags::IsDepthLinear);
        ID3D12Resource* inColor = desc.Resources.InColor;

        ComPtr<ID3D12Resource>& floorPing = m_outputBuffer1;
        ComPtr<ID3D12Resource>& floorPong = m_outputBuffer2;

        for (int i = 0; i < FloorSeed::kPasses; i++)
        {
            // The game's subrect origin only applies while the colour source is still
            // the game's texture. From pass 1 on it is the zero-based internal buffer,
            // so carrying the origin forward would shift every read. Depth keeps its
            // own origin because it is re-read from the game texture every pass.
            DirectX::XMUINT4 sourceBase = desc.FloorSourceBase;
            if (i > 0)
            {
                sourceBase.x = 0;
                sourceBase.y = 0;
            }

            FloorSeed::Constants constants = { .InvProjMatrix = desc.InvProjMatrix,
                                               .RenderSize = desc.RenderSize,
                                               .NearPlane = desc.NearPlane,
                                               .FarPlane = desc.FarPlane,
                                               .Flags = (isDepthLinear ? uint32_t(FloorSeed::Flags::LinearDepth) : 0u) |
                                                        ((desc.Flags & uint32_t(ConvFlags::RightHanded))
                                                             ? uint32_t(FloorSeed::Flags::NegativeViewDepth)
                                                             : 0u) |
                                                        // The title's published linear depth is authoritative for
                                                        // geometry: the canonical signed field is seeded from it so
                                                        // the floor filter, the packing shader and the denoiser's own
                                                        // depth input all describe the same view-space positions.
                                                        (((desc.Flags & uint32_t(ConvFlags::TitleLinearDepth)) != 0u &&
                                                          desc.Resources.InTitleLinearDepth != nullptr)
                                                             ? uint32_t(FloorSeed::Flags::TitleLinearDepth)
                                                             : 0u),
                                               .CurrentJitter = { desc.JitterOffsets.x, desc.JitterOffsets.y },
                                               .InputBase = sourceBase,
                                               .NormalBase = { desc.InputBase1.x, desc.InputBase1.y },
                                               ._NormalPadding = {},
                                               .TitleDepthBase = { desc.TitleLinearDepthBase.x,
                                                                   desc.TitleLinearDepthBase.y },
                                               ._TitleDepthPadding = {} };
            const auto cbData = GetAsByteSpan(constants);

            // Create median filtered raw color before cross bilateral filtering
            // Write to mip chain at top level
            FloorSeed::Input in = { .Resources = { .InColor = inColor,
                                                   .InNormals = desc.Resources.InNormals,
                                                   .InDepth = desc.Resources.InDepth,
                                                   .InTitleLinearDepth = desc.Resources.InTitleLinearDepth } };

            FloorSeed::Output out = { .Resources = { .OutColor = floorPing.Get(),
                                                     .OutLinearDepth = m_LinearDepth.Get(),
                                                     .OutDepthGradient = m_out.Resources.Motion.Get() } };

            m_floorSeedShader.Dispatch(cmdList, cbData, in.AsArray, out.AsArray, dispatchSize);

            std::swap(floorPing, floorPong);
            inColor = floorPong.Get();
        }

        m_smoothFloor = floorPong.Get();
    }

    void DispatchFloorFilter(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc)
    {
        const XMFLOAT2 dispatchSize = { desc.RenderSize.x, desc.RenderSize.y };

        ComPtr<ID3D12Resource>& floorPing = m_outputBuffer1;
        ComPtr<ID3D12Resource>& floorPong = m_outputBuffer2;

        // Tukey biweight: W = ( 1 - ( (center - tap) * scale )^2 )^2
        // scale = 2^(i + 1) / norm
        float rcpCrossNorm = (1.0f / 0.5f);
        float rcpLumNorm = (1e-2f / 0.3f);

        for (int i = 0; i < FloorFilter::kPasses; i++)
        {
            // The detail residual is only meaningful once the wavelet has reached its full
            // support; re-injecting it on every pass would compound it kPasses times.
            const bool isFinalPass = (i == (FloorFilter::kPasses - 1));

            FloorFilter::Constants constants = { .DstTexSize = desc.RenderSize,
                                                 .RcpCrossBlNorm = rcpCrossNorm,
                                                 .RcpSelfBlNorm = rcpLumNorm,
                                                 .StepSize = 1 << i,
                                                 .FrameIndex = m_floorFilterFrameIndex,
                                                 .DetailBoost = isFinalPass ? desc.FloorDetailBoost : 0.0f,
                                                 .NormalSharpness = desc.FloorNormalSharpness,
                                                 .AlbedoGuideStrength = desc.FloorAlbedoGuide,
                                                 .LumSymmetry = desc.FloorLumSymmetry,
                                                 .GrazingSharpness = desc.FloorGrazingSharpness,
                                                 .EnvelopeBias = desc.FloorEnvelopeBias,
                                                 // The guide is the only title texture this filter reads, so it is the
                                                 // only input whose subrect origin is not already zero.
                                                 .AlbedoBase = { desc.InputBase2.z, desc.InputBase2.w } };
            const auto cbData = GetAsByteSpan(constants);

            FloorFilter::Input in = { .Resources = { .InColor = m_smoothFloor,
                                                     .InLinearDepth = m_LinearDepth.Get(),
                                                     .InDepthGradient = m_out.Resources.Motion.Get(),
                                                     .InDiffAlbedo = desc.Resources.InDiffAlbedo } };

            FloorFilter::Output out = { .Resources = {
                                            // m_smoothFloor always references the pong buffer at the start of
                                            // an iteration. Write the opposite buffer, then swap the handles so
                                            // the freshly filtered result becomes the next iteration's input.
                                            .OutColor = floorPing.Get() } };

            m_floorFilterShader.Dispatch(cmdList, cbData, in.AsArray, out.AsArray, dispatchSize);

            std::swap(floorPing, floorPong);
            m_smoothFloor = floorPong.Get();
        }

        m_floorFilterFrameIndex++;
    }

    void DispatchPackingShader(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc)
    {
        const XMFLOAT2 dispatchSize = { desc.RenderSize.x, desc.RenderSize.y };

        // Prepare inputs for packing and format conversion
        Conversion::Input in = { .Resources = { .InColor = desc.Resources.InColor,
                                                .InDepth = m_LinearDepth.Get(),
                                                .InMotionVectors = desc.Resources.InMotionVectors,
                                                .InNormals = desc.Resources.InNormals,
                                                .InRoughness = desc.Resources.InRoughness,
                                                .InSpecHitDist = desc.Resources.InSpecHitDist,
                                                .InDiffAlbedo = desc.Resources.InDiffAlbedo,
                                                .InSpecAlbedo = desc.Resources.InSpecAlbedo,
                                                .InBiasMask = desc.Resources.InBiasMask,
                                                .InFloorColor = m_smoothFloor,
                                                .InInspector = desc.Resources.InInspector,
                                                .InEmissive = desc.Resources.InEmissive,
                                                .InSpecularRayDirectionHitDistance =
                                                    desc.Resources.InSpecularRayDirectionHitDistance,
                                                .InDiffuseHitDistance = desc.Resources.InDiffuseHitDistance,
                                                .InTitleLinearDepth = desc.Resources.InTitleLinearDepth,
                                                .InResponsivityMask = desc.Resources.InResponsivityMask } };

        uint32_t packFlags = desc.Flags | uint32_t(ConvFlags::IsDepthLinear);
        // A null SRV reads as zero, so the shader is safe either way, but the flag keeps the
        // "no mask provided" case explicit and visible in the debug views.
        if (desc.Resources.InBiasMask != nullptr)
            packFlags |= uint32_t(ConvFlags::HasBiasMask);
        if (desc.FloorHandoverMode != 0u)
            packFlags |= uint32_t(ConvFlags::FloorHandover);
        if (desc.Resources.InSpecularRayDirectionHitDistance && desc.SpecularHitDistanceFromCombinedAlpha)
        {
            packFlags |= uint32_t(ConvFlags::HasCombinedSpecHitDistance);
        }

        Conversion::Constants packConstants = {
            .InvViewMatrix = desc.InvViewMatrix,
            .InvProjMatrix = desc.InvProjMatrix,
            .PrevViewMatrix = desc.PrevViewMatrix,
            .DstTexSize = desc.RenderSize,
            .MotionInputSize = desc.MotionInputSize,
            .MotionTransform = desc.MotionTransform,
            .JitterOffsets = desc.JitterOffsets,
            .InputBase0 = desc.InputBase0,
            .InputBase1 = desc.InputBase1,
            .InputBase2 = desc.InputBase2,
            .InputBase3 = desc.InputBase3,
            .InputBase4 = desc.InputBase4,
            .InputBase5 = { desc.TitleLinearDepthBase.x, desc.TitleLinearDepthBase.y, desc.SpecularHitDistanceBase.x,
                            desc.SpecularHitDistanceBase.y },
            .NearPlane = desc.NearPlane,
            .FarPlane = desc.FarPlane,
            .FloorIsolation = desc.FloorIsolation,
            .RoughnessFloor = desc.RoughnessFloor,
            // The packing shader never sees the game's hardware depth. FloorSeed
            // has already converted it to signed linear view-space depth.
            .Flags = packFlags,
            .InspectorChannel = desc.InspectorChannel,
            .InspectorScale = desc.InspectorScale,
            .DebugDepthMax = desc.DebugDepthMax,
            // Only honour a mode the caller actually bound a resource for.
            .DiffuseHitDistanceMode = desc.Resources.InDiffuseHitDistance != nullptr ? desc.DiffuseHitDistanceMode : 0u,
            .FloorHandoverDetail = desc.FloorHandoverDetail,
            .FloorHandoverMode = desc.FloorHandoverMode,
            .ResponsivityTrustThreshold = desc.ResponsivityTrustThreshold,
            .ResponsivityInvert = desc.ResponsivityInvert ? 1u : 0u,
            ._Padding0 = 0.0f,
            ._Padding1 = 0.0f,
            .BiasMaskStrength = desc.BiasMaskStrength,
            .FloorSoftMin = desc.FloorSoftMin,
            .FloorHandoverStrength = desc.FloorHandoverStrength,
            .FloorClampSmoothing = desc.FloorClampSmoothing,
            .FloorRawBlend = desc.FloorRawBlend,
            .FloorStructureGate = desc.FloorStructureGate,
            .DemodDivisorFloor = desc.DemodDivisorFloor
        };

        const std::span<const byte> convCBData((const byte*) &packConstants, sizeof(packConstants));
        m_convShader.Dispatch(cmdList, convCBData, in.AsArray, m_out.AsRawArray, dispatchSize, true);
    }

    void DispatchConversion(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc)
    {
        if (!cmdList || !m_maxWidth)
            return;

        // A title-published linear depth reaches every consumer of view-space position
        // - the floor seed/filter, the packing shader and the denoiser's own depth
        // input - through the canonical signed copy the floor-seed pass writes into
        // m_LinearDepth, never as a raw hand-off: the descriptor carries no sign
        // convention, and a positive distance where an RH view matrix implies negative
        // view Z leaves RR unable to reproject anything. The raw resource stays bound
        // for this frame's own reads of it (the debug views that show it as published,
        // against the canonical copy), so its declared state is carried through rather
        // than assumed: declaring a wider state than the resource is in records an
        // invalid barrier.
        if ((desc.Flags & uint32_t(ConvFlags::TitleLinearDepth)) != 0 && desc.Resources.InTitleLinearDepth != nullptr)
        {
            m_rrLinearDepth = desc.Resources.InTitleLinearDepth;
            m_rrLinearDepthState = desc.TitleLinearDepthState;
            m_rrLinearDepthDeclaredState = desc.TitleLinearDepthDeclaredState;
        }
        else
        {
            m_rrLinearDepth = nullptr;
        }

        // The title's resource is in whatever state it declared - COMMON for titles that tag
        // without committing to one. Transition it in for the floor-seed and packing reads;
        // the caller hands it back once the denoiser has finished with it.
        m_rrLinearDepthForwarded = false;
        if (m_rrLinearDepth != nullptr && m_rrLinearDepthDeclaredState != static_cast<uint32_t>(kSrvState))
        {
            AddBarrier(cmdList, m_rrLinearDepth, static_cast<D3D12_RESOURCE_STATES>(m_rrLinearDepthDeclaredState),
                       kSrvState);
            m_rrLinearDepthForwarded = true;
        }

        TransitionDenoiserOutputsToRead(cmdList);

        // Filtered raster lighting estimate
        DispatchFloorSeed(cmdList, desc);
        DispatchFloorFilter(cmdList, desc);

        // DLSS-RR to FSR-RR conversion
        DispatchPackingShader(cmdList, desc);

        // Diagnostic: read back the RR-facing linear depth, motion and normals while
        // their state is still the one this code set. The denoiser dispatch below
        // records FFX's own barriers over these resources, so this must stay here.
        UpdateInputProbe(cmdList, desc);

        // Transition output buffers to UAV after last composition pass or first init.
        // The denoiser will be writing to these.
        {
            AddBarrier(cmdList, m_outputBuffer1.Get(), kSrvState, kUavState);
            AddBarrier(cmdList, m_outputBuffer2.Get(), kSrvState, kUavState);
            AddBarrier(cmdList, m_ambientOcclusionOutput.Get(), kSrvState, kUavState);
            AddBarrier(cmdList, m_specularOcclusionOutput.Get(), kSrvState, kUavState);
            m_radianceOutputsInUavState = true;
            m_ambientOcclusionOutputInUavState = true;
            m_specularOcclusionOutputInUavState = true;
        }
    }

    void DispatchComposition(ID3D12GraphicsCommandList* cmdList, const CompositionDesc& desc)
    {
        if (!cmdList || !m_maxWidth)
            return;

        auto& outResources = m_out.Resources;
        Composition::Input inputs = {};
        Composition::Constants constants = { .DstTexSize = desc.DstTexSize,
                                             .SourceBase = desc.SourceBase,
                                             .CorrelationBias = desc.CorrelationBias,
                                             .Flags = UINT(desc.Flags),
                                             .SourceUvScale = { 1.0f, 1.0f },
                                             .SourceUvOffset = {},
                                             .FloorHandoverAnchorClamp = desc.FloorHandoverAnchorClamp,
                                             .FloorHandoverCorrelationMix = desc.FloorHandoverCorrelationMix,
                                             ._Padding0 = {} };

        // Transition denoiser output buffers to SRV for composition.
        TransitionDenoiserOutputsToRead(cmdList);

        // A signal excluded from the denoiser chain (single-signal denoise mode) is
        // never written this frame, and its ping-pong buffer is shared with the floor
        // passes - so it still holds that floor's intermediate image. Multiplied by
        // the albedo it would add floor light to the disabled channel instead of the
        // signal. The packed raw signal is the demodulated radiance the denoiser
        // would otherwise have consumed, so it is the energy-preserving passthrough.
        ID3D12Resource* const specularRadiance = (desc.Flags & (uint32_t) CompFlags::SpecularSignalDisabled) != 0
                                                     ? outResources.Signals.IndirectSpecular.Get()
                                                     : m_outputBuffer1.Get();
        ID3D12Resource* const diffuseRadiance = (desc.Flags & (uint32_t) CompFlags::DiffuseSignalDisabled) != 0
                                                    ? outResources.Signals.DirectDiffuse.Get()
                                                    : m_outputBuffer2.Get();

        inputs.Resources = { .InIndirectSpecular = specularRadiance,
                             .InSpecularAlbedo = outResources.SpecAlbedo.Get(),
                             .InDirectDiffuse = diffuseRadiance,
                             .InDiffuseAlbedo = outResources.DiffAlbedo.Get(),
                             .InSkipSignal = outResources.SkipSignal.Get(),
                             .InRawColor = desc.InRawColor,
                             .InRawIndirectSpecular = outResources.Signals.IndirectSpecular.Get(),
                             .InNormals = outResources.Normals.Get(),
                             .InHandover = outResources.Handover.Get() };

        // Motion stays a pure motion-vector texture: writing the composed colour there
        // would feed the denoiser its own output as motion vectors.
        ID3D12Resource* const compositionTarget = m_out.Resources.Motion.Get();
        std::array<ID3D12Resource*, 1> uavs { compositionTarget };
        const std::span<const byte> cbData((const byte*) &constants, sizeof(constants));
        const XMFLOAT2 dstDim = { constants.DstTexSize.x, constants.DstTexSize.y };

        m_compShader.Dispatch(cmdList, cbData, inputs.AsArray, uavs, dstDim, true);
    }

    void TransitionDenoiserOutputsToRead(ID3D12GraphicsCommandList* cmdList) noexcept
    {
        if (!cmdList)
            return;

        if (m_radianceOutputsInUavState)
        {
            std::array<ID3D12Resource*, 2> buffers = { m_outputBuffer1.Get(), m_outputBuffer2.Get() };
            AddBarriers(cmdList, buffers, kUavState, kSrvState);
            m_radianceOutputsInUavState = false;
        }

        if (m_ambientOcclusionOutputInUavState)
        {
            AddBarrier(cmdList, m_ambientOcclusionOutput.Get(), kUavState, kSrvState);
            m_ambientOcclusionOutputInUavState = false;
        }

        if (m_specularOcclusionOutputInUavState)
        {
            AddBarrier(cmdList, m_specularOcclusionOutput.Get(), kUavState, kSrvState);
            m_specularOcclusionOutputInUavState = false;
        }
    }

    void TransitionDenoiserOutputsToUav(ID3D12GraphicsCommandList* cmdList) noexcept
    {
        if (!cmdList)
            return;

        if (!m_radianceOutputsInUavState)
        {
            std::array<ID3D12Resource*, 2> buffers = { m_outputBuffer1.Get(), m_outputBuffer2.Get() };
            AddBarriers(cmdList, buffers, kSrvState, kUavState);
            m_radianceOutputsInUavState = true;
        }

        if (!m_ambientOcclusionOutputInUavState)
        {
            AddBarrier(cmdList, m_ambientOcclusionOutput.Get(), kSrvState, kUavState);
            m_ambientOcclusionOutputInUavState = true;
        }

        if (!m_specularOcclusionOutputInUavState)
        {
            AddBarrier(cmdList, m_specularOcclusionOutput.Get(), kSrvState, kUavState);
            m_specularOcclusionOutputInUavState = true;
        }
    }

    void CopyAmbientOcclusionOutput(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* dstTex,
                                    D3D12_RESOURCE_STATES dstState, uint32_t logicalWidth, uint32_t logicalHeight)
    {
        if (!cmdList || !dstTex || !m_ambientOcclusionOutputInUavState || logicalWidth == 0 || logicalHeight == 0)
            throw std::runtime_error("Ambient-occlusion output is unavailable for publication");

        const D3D12_RESOURCE_DESC srcDesc = m_ambientOcclusionOutput->GetDesc();
        const D3D12_RESOURCE_DESC dstDesc = dstTex->GetDesc();
        if (srcDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            dstDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || logicalWidth > srcDesc.Width ||
            logicalHeight > srcDesc.Height || logicalWidth > dstDesc.Width || logicalHeight > dstDesc.Height ||
            dstDesc.DepthOrArraySize != 1 || dstDesc.MipLevels != 1)
        {
            throw std::runtime_error("Ambient-occlusion destination is incompatible with the allocation ceiling");
        }

        AddBarrier(cmdList, m_ambientOcclusionOutput.Get(), kUavState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (dstState != D3D12_RESOURCE_STATE_COPY_DEST)
            AddBarrier(cmdList, dstTex, dstState, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_TEXTURE_COPY_LOCATION srcLocation {};
        srcLocation.pResource = m_ambientOcclusionOutput.Get();
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLocation.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dstLocation {};
        dstLocation.pResource = dstTex;
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLocation.SubresourceIndex = 0;
        const D3D12_BOX sourceBox { 0, 0, 0, logicalWidth, logicalHeight, 1 };
        cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, &sourceBox);

        AddBarrier(cmdList, m_ambientOcclusionOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kSrvState);
        if (dstState != D3D12_RESOURCE_STATE_COPY_DEST)
            AddBarrier(cmdList, dstTex, D3D12_RESOURCE_STATE_COPY_DEST, dstState);

        m_ambientOcclusionOutputInUavState = false;
    }

    ID3D12Resource* PrepareDebugViewOutput(ID3D12GraphicsCommandList* cmdList, UINT width, UINT height)
    {
        if (!cmdList || !m_pDev || width == 0 || height == 0)
            throw std::runtime_error("Invalid AMD RR debug-view output request");

        if (!m_debugViewOutput || m_debugViewWidth != width || m_debugViewHeight != height)
        {
            auto newOutput =
                CreateTexture2D(m_pDev, width, height, FSRDFormats::DebugView, L"FSR_RR_DebugView_Output", kUavState);
            // Retire rather than release. The ring matches this preprocessor's
            // frames-in-flight reuse horizon even when DRS reallocates every frame.
            m_retiredDebugViewOutputs[m_retiredDebugViewOutputIndex] = std::move(m_debugViewOutput);
            m_retiredDebugViewOutputIndex = (m_retiredDebugViewOutputIndex + 1u) % kBackBufferCount;
            m_debugViewOutput = std::move(newOutput);
            m_debugViewWidth = width;
            m_debugViewHeight = height;
            m_debugViewOutputInUavState = true;

            LOG_INFO("[RR_DIAG] created dedicated AMD debug-view output: {}x{}, "
                     "DXGI_FORMAT_R16G16B16A16_FLOAT, UAV",
                     width, height);
        }
        else if (!m_debugViewOutputInUavState)
        {
            AddBarrier(cmdList, m_debugViewOutput.Get(), kSrvState, kUavState);
            m_debugViewOutputInUavState = true;
        }

        return m_debugViewOutput.Get();
    }

    void TransitionDebugViewOutputToRead(ID3D12GraphicsCommandList* cmdList) noexcept
    {
        if (!cmdList || !m_debugViewOutput || !m_debugViewOutputInUavState)
            return;

        AddBarrier(cmdList, m_debugViewOutput.Get(), kUavState, kSrvState);
        m_debugViewOutputInUavState = false;
    }

    void Blit(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* srcTex, ID3D12Resource* dstTex, XMFLOAT2 dstDim,
              XMFLOAT2 logicalSrcDim, XMFLOAT2 logicalSrcBase)
    {
        if (!cmdList || !srcTex || !dstTex)
            return;

        const D3D12_RESOURCE_DESC srcDesc = srcTex->GetDesc();
        const XMFLOAT2 physicalSrcDim { static_cast<float>(srcDesc.Width), static_cast<float>(srcDesc.Height) };
        if (logicalSrcDim.x == 0.0f || logicalSrcDim.y == 0.0f)
            logicalSrcDim = physicalSrcDim;

        if (dstDim.x == 0 || dstDim.y == 0)
        {
            D3D12_RESOURCE_DESC dstDesc = dstTex->GetDesc();
            dstDim.x = (float) dstDesc.Width;
            dstDim.y = (float) dstDesc.Height;
        }

        if (physicalSrcDim.x == 0.0f || physicalSrcDim.y == 0.0f || logicalSrcDim.x <= 0.0f ||
            logicalSrcDim.y <= 0.0f || logicalSrcBase.x < 0.0f || logicalSrcBase.y < 0.0f ||
            logicalSrcBase.x + logicalSrcDim.x > physicalSrcDim.x ||
            logicalSrcBase.y + logicalSrcDim.y > physicalSrcDim.y || dstDim.x <= 0.0f || dstDim.y <= 0.0f)
            return;

        Composition::Input inputs = {};
        inputs.Resources.InIndirectSpecular = srcTex;

        const Composition::Constants constants = {
            .DstTexSize = { dstDim.x, dstDim.y, (1.0f / dstDim.x), (1.0f / dstDim.y) },
            .Flags = (UINT) CompFlags::RawSourceBlit | (UINT) CompFlags::ScaleSrc,
            .SourceUvScale = { logicalSrcDim.x / physicalSrcDim.x, logicalSrcDim.y / physicalSrcDim.y },
            .SourceUvOffset = { logicalSrcBase.x / physicalSrcDim.x, logicalSrcBase.y / physicalSrcDim.y }
        };

        std::array<ID3D12Resource*, 1> uavs { dstTex };
        const std::span<const byte> cbData((const byte*) &constants, sizeof(constants));

        m_compShader.Dispatch(cmdList, cbData, inputs.AsArray, uavs, dstDim, false);
    }

    void SetDescResources(ffxDispatchDescHeader& signalHeader, ffxDispatchDescDenoiser& dispatchDesc)
    {
        auto& outResources = m_out.Resources;

        dispatchDesc.header = {
            .type = FFX_API_DISPATCH_DESC_TYPE_DENOISER,
            .pNext = &signalHeader // Link signal desc to main header
        };

        // Always the converter's own field: it is the one whose sign convention is known,
        // and the one every other consumer reads - the floor seed writes it from the
        // title's published linear depth when one is provided, so the denoiser sees the
        // same geometry the packing and floor passes describe.
        dispatchDesc.linearDepth =
            ffxApiGetResourceDX12(m_LinearDepth.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatchDesc.motionVectors =
            ffxApiGetResourceDX12(outResources.Motion.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatchDesc.normals =
            ffxApiGetResourceDX12(outResources.Normals.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatchDesc.specularAlbedo =
            ffxApiGetResourceDX12(outResources.SpecAlbedo.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatchDesc.diffuseAlbedo =
            ffxApiGetResourceDX12(outResources.DiffAlbedo.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    }
};

// Public interface

FSRDPreprocessor_Dx12::FSRDPreprocessor_Dx12(std::string_view name, ID3D12Device* pDev)
    : m_impl(std::make_unique<Impl>()), m_InstanceName(name), m_IsInitialized(false)
{
    try
    {
        m_impl->m_pDev = pDev;
        m_impl->Initialize(GetAsByteSpan(FSRDFloorSeed_cso), GetAsByteSpan(FSRDFloor_cso),
                           GetAsByteSpan(FSRDInputConv_cso), GetAsByteSpan(FSRDOutputComp_cso));
        m_IsInitialized = true;
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("FSRD shaders failed to initialize. Details: {}", err.what());
    }
}

FSRDPreprocessor_Dx12::~FSRDPreprocessor_Dx12() = default;

bool FSRDPreprocessor_Dx12::IsInit() const { return m_IsInitialized; }

std::string_view FSRDPreprocessor_Dx12::GetName() const { return m_InstanceName; }

bool FSRDPreprocessor_Dx12::SetMaxRenderSize(UINT width, UINT height)
{
    try
    {
        m_impl->SetMaxRenderSize(width, height);
        return true;
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("Failed to resize FSRD buffers. Details: {}", err.what());
    }

    return false;
}

bool FSRDPreprocessor_Dx12::DispatchConversion(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc)
{
    try
    {
        m_impl->DispatchConversion(cmdList, desc);
        return true;
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("FSRD input conversion failed. Details: {}", err.what());
    }

    return false;
}

void FSRDPreprocessor_Dx12::GetSignals(ffxDispatchDescDenoiser& dispatchDesc,
                                       ffxDispatchDescDenoiserDirectDiffuse& directDiffuse,
                                       ffxDispatchDescDenoiserIndirectSpecular& indirectSpecular) const
{
    auto& outResources = m_impl->m_out.Resources;
    auto& signalData = outResources.Signals;
    ID3D12Resource* diffuseInput = signalData.DirectDiffuse.Get();
    ID3D12Resource* specularInput = signalData.IndirectSpecular.Get();

    directDiffuse = {
        .header = { .type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE, .pNext = &indirectSpecular.header },
        .signal = { .input = ffxApiGetResourceDX12(diffuseInput, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ),
                    .output =
                        ffxApiGetResourceDX12(m_impl->m_outputBuffer2.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS),
                    .checkerboardOrigin = 0 }
    };

    indirectSpecular = { .header = { .type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR },
                         .signal = {
                             .input = ffxApiGetResourceDX12(specularInput, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ),
                             .output = ffxApiGetResourceDX12(m_impl->m_outputBuffer1.Get(),
                                                             FFX_API_RESOURCE_STATE_UNORDERED_ACCESS),
                             .checkerboardOrigin = 0 } };

    m_impl->SetDescResources(directDiffuse.header, dispatchDesc);
}

bool FSRDPreprocessor_Dx12::DispatchComposition(ID3D12GraphicsCommandList* cmdList, const CompositionDesc& desc)
{
    try
    {
        m_impl->DispatchComposition(cmdList, desc);
        return true;
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("FSRD output composition failed. Details: {}", err.what());
    }

    return false;
}

void FSRDPreprocessor_Dx12::TransitionDenoiserOutputsToRead(ID3D12GraphicsCommandList* cmdList) noexcept
{
    m_impl->TransitionDenoiserOutputsToRead(cmdList);
}

void FSRDPreprocessor_Dx12::TransitionDenoiserOutputsToUav(ID3D12GraphicsCommandList* cmdList) noexcept
{
    m_impl->TransitionDenoiserOutputsToUav(cmdList);
}

void FSRDPreprocessor_Dx12::RestoreTitleInputStates(ID3D12GraphicsCommandList* cmdList) noexcept
{
    if (m_impl->m_rrLinearDepthForwarded && m_impl->m_rrLinearDepth != nullptr && cmdList != nullptr)
    {
        AddBarrier(cmdList, m_impl->m_rrLinearDepth, kSrvState,
                   static_cast<D3D12_RESOURCE_STATES>(m_impl->m_rrLinearDepthDeclaredState));
        m_impl->m_rrLinearDepthForwarded = false;
    }
}

bool FSRDPreprocessor_Dx12::CopyAmbientOcclusionOutput(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* dstTex,
                                                       D3D12_RESOURCE_STATES dstState, uint32_t logicalWidth,
                                                       uint32_t logicalHeight)
{
    try
    {
        m_impl->CopyAmbientOcclusionOutput(cmdList, dstTex, dstState, logicalWidth, logicalHeight);
        return true;
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("FSRD ambient-occlusion publication failed. Details: {}", err.what());
    }

    return false;
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetAmbientOcclusionOutput() const
{
    return m_impl->m_ambientOcclusionOutput.Get();
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetSpecularOcclusionOutput() const
{
    return m_impl->m_specularOcclusionOutput.Get();
}

ID3D12Resource* FSRDPreprocessor_Dx12::PrepareDebugViewOutput(ID3D12GraphicsCommandList* cmdList, uint32_t width,
                                                              uint32_t height)
{
    try
    {
        return m_impl->PrepareDebugViewOutput(cmdList, width, height);
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("Failed to prepare AMD RR debug-view output. Details: {}", err.what());
    }

    return nullptr;
}

void FSRDPreprocessor_Dx12::TransitionDebugViewOutputToRead(ID3D12GraphicsCommandList* cmdList) noexcept
{
    m_impl->TransitionDebugViewOutputToRead(cmdList);
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetDebugViewOutput() const { return m_impl->m_debugViewOutput.Get(); }

ID3D12Resource* FSRDPreprocessor_Dx12::GetCompositionOutput() const
{
    // Motion stays a pure motion-vector texture: writing the composed colour there would
    // feed the denoiser its own output as motion vectors.
    return m_impl->m_out.Resources.Motion.Get();
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetDenoiserDiffuseOutput() const { return m_impl->m_outputBuffer2.Get(); }

ID3D12Resource* FSRDPreprocessor_Dx12::GetDenoiserDiffuseInputSignal() const
{
    return m_impl->m_out.Resources.Signals.DirectDiffuse.Get();
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetDenoiserSpecularOutput() const { return m_impl->m_outputBuffer1.Get(); }

ID3D12Resource* FSRDPreprocessor_Dx12::GetDenoiserNormalsInput() const { return m_impl->m_out.Resources.Normals.Get(); }

ID3D12Resource* FSRDPreprocessor_Dx12::GetDenoiserMotionInput() const { return m_impl->m_out.Resources.Motion.Get(); }

ID3D12Resource* FSRDPreprocessor_Dx12::GetDenoiserLinearDepthInput() const { return m_impl->m_LinearDepth.Get(); }

bool FSRDPreprocessor_Dx12::Blit(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* srcTex, ID3D12Resource* dstTex,
                                 XMFLOAT2 dstDim, XMFLOAT2 logicalSrcDim, XMFLOAT2 logicalSrcBase) const

{
    try
    {
        m_impl->Blit(cmdList, srcTex, dstTex, dstDim, logicalSrcDim, logicalSrcBase);
        return true;
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("FSRD blit failed. Details: {}", err.what());
    }

    return false;
}
