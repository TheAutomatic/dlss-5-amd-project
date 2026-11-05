#include "pch.h"
// Capture/readback safety rides the tree's own submission retirement: a GpuBatch
// ticket recorded on the capturing command list retires only when the queue batch
// completed, or the recording was provably discarded before reaching the GPU.
#include <dlssnr/submission/GpuBatchTracker.h>
#include "RetiredSlotReservations.h"
#include "RRTraceAdditiveIO.h"
#include "precompile/RRTraceAdditive_Shader.h"
#include "shaders/RecordedShader_Dx12.h"
#include "Util.h"
#include <optional>
#include <mutex>
#include "FSRDPreprocessor_Dx12.h"
#include "gpu_time/FSRDStageTimings_Dx12.h"
#include "FSRDShaderUtils.h"
#include "FSRDShaderData.h"
#include "FSRDCompositionVariant.h"
#include "FSRDCompositionHistory.h"
#include "precompile/FSRDInputConv_Shader.h" 
#include "precompile/FSRDInputConvAdditive_Shader.h"
#include "precompile/FSRDFloorSeed_Shader.h" 
#include "precompile/FSRDFloor_Shader.h" 
#include "precompile/FSRDOutputComp_Shader.h"
#include "precompile/FSRDOutputCompLight_Shader.h"
#include "precompile/FSRDOutputCompNoRecovery_Shader.h"
#include "precompile/FSRDOutputCompTileLight_Shader.h"
#include "precompile/FSRDOutputCompTileAnchor_Shader.h"
#include "precompile/FSRDUpscalerDepth_Shader.h"
#include "precompile/FSRDAlbedoTrustEvidence_Shader.h"
#include "precompile/FSRDAlbedoTrustPropagate_Shader.h"

#include "dx12/ffx_api_dx12.h"
#include "fsr-rr/ffx_denoiser.h"

#include <d3dcompiler.h>
#include <d3d12.h>
#include <stdexcept>
#include <limits>
#include <cstddef>
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

// Keep the portable pipeline selector tied to the actual shader flag contract.
static_assert(uint32_t(FSRDPreprocessor_Dx12::CompFlags::RawSourceBlit) == 1u);
static_assert(uint32_t(FSRDPreprocessor_Dx12::CompFlags::Debug) == (1u << 16));

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

    // Cleaned HDR reference plus sigma; alpha -1 marks explicit detail bypass.
    constexpr DXGI_FORMAT DetailReference = SkipSignal;

    constexpr DXGI_FORMAT OutputBuffer1 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT OutputBuffer2 = DXGI_FORMAT_R16G16B16A16_FLOAT;

    constexpr DXGI_FORMAT AmbientOcclusion = DXGI_FORMAT_R8_UNORM;
    constexpr DXGI_FORMAT SpecularOcclusion = DXGI_FORMAT_R8_UNORM;
    constexpr DXGI_FORMAT DebugView = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // Unsupported-albedo recovery: RR direct-specular input/output and the
    // (unsupported, structure) vote sums. Six five-tap propagation steps bound the sums at
    // 5^6 = 15625, inside FP16; each pass accumulates in FP32 and composition reads only
    // their ratio and the structure mass. At 1440p half storage cut the propagation passes
    // from 0.80 to 0.71 ms (RX 9070) and moved the blend weight by at most 0.0023.
    constexpr DXGI_FORMAT DirectSpecular = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr DXGI_FORMAT AlbedoTrust = DXGI_FORMAT_R16G16_FLOAT;
}

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
    struct UploadPage
    {
        ComPtr<ID3D12Resource> buffer;
        byte* mapped = nullptr;
        ~UploadPage() { if (buffer && mapped) buffer->Unmap(0, nullptr); }
    };
    struct Slot
    {
        std::shared_ptr<UploadPage> page;
        UINT offset = 0;
        FrameDescriptorHeap heap;
        std::weak_ptr<RecordedShader::Owner> recording;
    };
    // Growth never moves or replaces allocations already referenced by GPU work.
    std::vector<std::shared_ptr<Slot>> m_slots;
    // Dispatch slots whose constant memory and descriptors may still be read by an
    // outstanding queue batch are only reused once that batch's GpuBatch ticket
    // retires (completed, or provably discarded before reaching the GPU).
    struct SlotTicket
    {
        DlssNr::Submission::GpuBatch::Tracker::Ticket gpu = {};
        Slot* slot = nullptr;
        bool operator==(const SlotTicket& other) const { return gpu == other.gpu && slot == other.slot; }
    };
    RetiredSlotReservations<SlotTicket> m_reservations;
    struct SlotOwner final : RecordedShader::Owner
    {
        std::shared_ptr<Slot> slot;
        std::shared_ptr<void> retention;
        std::vector<ComPtr<ID3D12Resource>> resources;
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pipeline;
    };
    UINT m_cbSlotSize = 0;
    UINT m_pageSlots = 0;
    UINT m_numSrvs = 0;
    UINT m_numUavs = 0;
    // The shader's [numthreads]; Dispatch covers outDim with groups of this size.
    UINT m_groupX = kThreadGroupSizeX, m_groupY = kThreadGroupSizeY;
    std::wstring m_name;
    static constexpr size_t kMaxSlots = 256; // bound memory if submissions stop retiring

    void AddPage()
    {
        const UINT count = static_cast<UINT>(std::min<size_t>(m_pageSlots, kMaxSlots - m_slots.size()));
        if (count == 0)
            throw std::runtime_error("FSRD slot pool exhausted (256 slots still protected): " +
                                     std::string(m_name.begin(), m_name.end()));
        auto page = std::make_shared<UploadPage>();
        D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = UINT64(m_cbSlotSize) * count;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(m_pDev->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(&page->buffer)),
                      "Failed to create FSRD constant page");
        page->buffer->SetName(m_name.c_str());
        D3D12_RANGE readRange { 0, 0 };
        ThrowIfFailed(page->buffer->Map(0, &readRange, reinterpret_cast<void**>(&page->mapped)),
                      "Failed to map FSRD constant page");

        // Finish all fallible allocations before publishing any part of this page.
        std::vector<std::shared_ptr<Slot>> slots;
        slots.reserve(count);
        for (UINT i = 0; i < count; ++i)
        {
            auto slot = std::make_shared<Slot>();
            slot->page = page;
            slot->offset = i * m_cbSlotSize;
            if (!slot->heap.Initialize(m_pDev, m_numSrvs, m_numUavs, 0, 0))
                throw std::runtime_error("Failed to initialize FSRD slot descriptors");
            slots.push_back(std::move(slot));
        }
        m_slots.reserve(m_slots.size() + count);
        for (auto& slot : slots)
            m_slots.push_back(std::move(slot));
    }

    void Reserve(UINT count)
    {
        auto& tracker = DlssNr::Submission::GpuBatch::Tracker::Instance();
        m_reservations.Reserve(count, [&tracker](const SlotTicket& ticket) {
                return ticket.slot->recording.expired() && tracker.Retired(ticket.gpu);
            },
            [this](size_t target) {
                if (target > kMaxSlots)
                    throw std::runtime_error("FSRD slot reservation exceeds safe pool capacity: " +
                                             std::string(m_name.begin(), m_name.end()));
                while (m_slots.size() < target)
                    AddPage();
                return m_slots.size();
            });
    }

    void Initialize(ID3D12Device* pDev, std::span<const byte> bytecode, UINT cbDataSize, UINT numSrvs, UINT numUavs,
                    LPCWSTR cbName, UINT backBufferCount = kBackBufferCount)
    {
        m_pDev = pDev;
        m_pageSlots = std::max(1u, backBufferCount);
        m_numSrvs = numSrvs;
        m_numUavs = numUavs;
        m_name = cbName;

        // Create Root Signature
        ThrowIfFailed(m_pDev->CreateRootSignature(0, bytecode.data(), bytecode.size(), IID_PPV_ARGS(&m_rootSig)),
                      "Failed to create Root Signature");

        // Create PSO
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = m_rootSig.Get();
        psoDesc.CS = { bytecode.data(), bytecode.size() };
        ThrowIfFailed(m_pDev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso)), "Failed to create PSO");

        m_cbSlotSize = AlignTo256(cbDataSize);
        AddPage();
    }

    std::shared_ptr<Slot> Dispatch(ID3D12GraphicsCommandList* cmdList, std::span<const byte> cbData,
                  std::span<ID3D12Resource* const> inputs, std::span<const MipChainDesc> inputMips,
                  std::span<ID3D12Resource*> output, std::span<const UINT> outputMips, XMFLOAT2 outDim,
                  bool autoBarrierOutput = true, ID3D12PipelineState* pipelineState = nullptr,
                  std::shared_ptr<void> retention = {})
    {
        if (!cmdList)
            return {};

        ScopedSkipHeapCapture skipHeapCapture {};

        // Standalone blits also use the pool; the normal conversion preflights
        // every stage before the first command is recorded.
        if (!m_reservations.HasReserved())
            Reserve(1);
        if (cbData.size() > m_cbSlotSize)
            throw std::runtime_error("FSRD constants exceed slot size");
        auto& tracker = DlssNr::Submission::GpuBatch::Tracker::Instance();
        const auto slotIndex = m_reservations.NextReserved();
        const auto retainedSlot = m_slots[slotIndex];
        auto& slot = *retainedSlot;
        // Completed is not reusable while the closed recording can replay.
        // The recording and submitted Owner closure pin the exact CB/heap pair.
        if (RecordedShader::Supported(cmdList))
        {
            auto owner = std::make_shared<SlotOwner>();
            owner->device = m_pDev;
            owner->slot = retainedSlot;
            owner->retention = std::move(retention);
            owner->root = m_rootSig;
            owner->pipeline = pipelineState ? pipelineState : m_pso.Get();
            owner->resources.reserve(inputs.size() + output.size());
            for (auto* resource : inputs) owner->resources.emplace_back(resource);
            for (auto* resource : output) owner->resources.emplace_back(resource);
            if (!owner->Attach(cmdList)) throw std::runtime_error("FSRD slot recording observer unavailable");
            slot.recording = owner;
        }
        m_reservations.Commit({ tracker.Record(cmdList), &slot });
        memcpy(slot.page->mapped + slot.offset, cbData.data(), cbData.size());
        const D3D12_GPU_VIRTUAL_ADDRESS cbAddress = slot.page->buffer->GetGPUVirtualAddress() + slot.offset;
        // Transitions SRV -> UAV
        if (autoBarrierOutput)
            AddBarriers(cmdList, output, outputMips, kSrvState, kUavState);

        // Update descriptors
        FrameDescriptorHeap& currentHeap = slot.heap;
        CreateSRVs(m_pDev, currentHeap, inputs, inputMips);
        CreateUAVs(m_pDev, currentHeap, output, outputMips);

        // Configure pipeline. Composition variants and the additive conversion share
        // this root signature, descriptor layout and constant slot; only the PSO differs.
        cmdList->SetPipelineState(pipelineState ? pipelineState : m_pso.Get());
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
        const UINT dimX = ((UINT) outDim.x + (m_groupX - 1)) / m_groupX;
        const UINT dimY = ((UINT) outDim.y + (m_groupY - 1)) / m_groupY;
        cmdList->Dispatch(dimX, dimY, 1);

        // Transition the UAVs back to SRV
        if (autoBarrierOutput)
            AddBarriers(cmdList, output, outputMips, kUavState, kSrvState);
        return retainedSlot;
    }

    std::shared_ptr<Slot> Dispatch(ID3D12GraphicsCommandList* cmdList, std::span<const byte> cbData,
                  std::span<ID3D12Resource* const> inputs, std::span<ID3D12Resource*> output, XMFLOAT2 outDim,
                  bool autoBarrierOutput = true, ID3D12PipelineState* pipelineState = nullptr,
                  std::shared_ptr<void> retention = {})
    {
        return Dispatch(cmdList, cbData, inputs, {}, output, {}, outDim, autoBarrierOutput, pipelineState,
                        std::move(retention));
    }
};


namespace {
std::mutex g_additiveTraceMutex;
std::optional<std::array<UINT,3>> g_additiveTraceRequest;
bool g_additiveTraceBusy=false;
std::string g_additiveTraceStatus="No additive channel capture requested.";
constexpr const char* kAdditiveFieldNames[] = {"rejected","evaluated","eligible","preflight_count","fit_count","mean_albedo","variance_albedo","min_specular","max_specular","mean_specular","mean_residual","variance_residual","covariance","ridge_lambda","data_weight","prior_slope","unregularized_slope","ridge_slope","intercept_unclamped","intercept","intercept_error","fit_rmse","applied_slope","applied_intercept","center_prediction","center_prediction_error","p0","p","delta_p","transferred_rgb","eligible_rgb","skip_rgb","skip_fraction","remodulated_fraction","raw_rgb","spatial_floor_rgb","eligible_fraction","specular_signal","diffuse_signal","settings","source_diffuse","source_specular","source_normal","source_roughness","linear_depth","source_motion","stored_specular","stored_diffuse"};
}
bool FSRDPreprocessor_Dx12::RequestAdditiveCapture(uint32_t x,uint32_t y,uint32_t size)
{
    std::scoped_lock lock(g_additiveTraceMutex);
    if (g_additiveTraceBusy || g_additiveTraceRequest || size<16 || size>128) return false;
    g_additiveTraceRequest=std::array<UINT,3>{x,y,size};
    g_additiveTraceStatus="Additive capture requested; waiting for FSR-RR conversion.";
    return true;
}
std::string FSRDPreprocessor_Dx12::GetAdditiveCaptureStatus()
{
    std::scoped_lock lock(g_additiveTraceMutex);
    return g_additiveTraceStatus;
}

// Private implementation
struct FSRDPreprocessor_Dx12::Impl
{
    ID3D12Device* m_pDev = nullptr;
#include "RRTraceAdditive.inl"
    ~Impl()
    {
        ReleaseInputProbe();
        if (m_additiveCapture)
        {
            std::scoped_lock lock(g_additiveTraceMutex);
            g_additiveTraceBusy=false;
            g_additiveTraceStatus="Additive capture owner ended; unfinished capture not exported.";
        }
    }

    ComputeState m_floorSeedShader;
    ComputeState m_floorFilterShader;
    ComputeState m_convShader;
    ComputeState m_compShader;
    // Reuse generic root signature, descriptors and leased constants/resources.
    ComPtr<ID3D12PipelineState> m_lightCompPso;
    ComPtr<ID3D12PipelineState> m_noRecoveryCompPso;
    bool m_lightCompPsoFailed = false;
    bool m_noRecoveryCompPsoFailed = false;
    ComPtr<ID3D12PipelineState> m_tileLightCompPso;
    ComPtr<ID3D12PipelineState> m_tileAnchorCompPso;
    bool m_splitCompPsoFailed = false;
    ComputeState m_trustEvidenceShader;
    ComputeState m_trustPropagateShader;
    // Shares conversion's root signature, per-dispatch descriptors and constants.
    // The original PSO remains usable if optional pipeline creation fails.
    ComPtr<ID3D12PipelineState> m_additiveConvPso;
    bool m_additiveConvPsoFailed = false;

    UINT m_maxWidth = 0;
    UINT m_maxHeight = 0;

    // Output Targets
    // Internal storage
    Conversion::Output m_out;
    ComPtr<ID3D12Resource> m_LinearDepth;
    ComPtr<ID3D12Resource> m_canonicalDepth;
    ComPtr<ID3D12Resource> m_floorReference;
    ComPtr<ID3D12Resource> m_compositionOutput;
    using HistoryGeneration = FSRD::CompositionHistory::Generation;
    using HistoryUse = FSRD::CompositionHistory::Use;
    struct CompositionRead final : HistoryUse
    {
        std::array<std::shared_ptr<ComputeState::Slot>, 2> slots;
        UINT count = 0;
        void EnableHistory() noexcept
        {
            for (UINT i = 0; i < count; ++i)
            {
                auto& slot = *slots[i];
                const UINT enabled = 1;
                memcpy(slot.page->mapped + slot.offset + offsetof(Composition::Constants, HistoryValid),
                       &enabled, sizeof(enabled));
                CreateSRV(device.Get(), slot.heap.GetSrvCPU(9), generation->decisions.Get());
                CreateSRV(device.Get(), slot.heap.GetSrvCPU(10), generation->metadata.Get());
            }
            selection = Selection::Pending;
        }
        bool CanFinalizeNoHistory() const noexcept override
        {
            if (!count || count > slots.size() || submittedOnce) return false;
            for (UINT i = 0; i < count; ++i)
                if (!slots[i] || slots[i]->recording.expired()) return false;
            return true;
        }
        void FinalizeNoHistory() noexcept override
        {
            for (UINT i = 0; i < count; ++i)
            {
                auto& slot = *slots[i];
                const UINT disabled = 0;
                memcpy(slot.page->mapped + slot.offset + offsetof(Composition::Constants, HistoryValid),
                       &disabled, sizeof(disabled));
                // Match the shader's typed SRVs; metadata is uint4, not UNORM.
                D3D12_SHADER_RESOURCE_VIEW_DESC view {};
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                view.Texture2D.MipLevels = 1;
                view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                device->CreateShaderResourceView(nullptr, &view, slot.heap.GetSrvCPU(9));
                view.Format = DXGI_FORMAT_R32G32B32A32_UINT;
                device->CreateShaderResourceView(nullptr, &view, slot.heap.GetSrvCPU(10));
            }
        }
    };
    std::vector<std::shared_ptr<HistoryGeneration>> m_historyGenerations;
    std::shared_ptr<HistoryGeneration> m_historyPrevious, m_historyNext;
    bool m_historyValid = false;
    bool m_historyPending = false;
    bool m_motionHistoryValid = false;
    XMFLOAT2 m_historyJitterDelta {};
    std::array<XMUINT4,7> m_historySourceBases {};
    XMFLOAT4 m_historyRenderSize {};
    // SR encoded-depth output for this evaluation; regenerated by
    // PrepareUpscalerDepthInput after a conversion produced the canonical
    // linear depth. Impl-owned, protected until the recording GpuBatch retires.
    ComputeState m_upscalerDepthShader;
    ComPtr<ID3D12Resource> m_upscalerDepthOutput;
    bool m_upscalerDepthValid = false;
    XMFLOAT4 m_upscalerDepthRenderSize {};
    bool m_canonicalDepthValid = false;
    ID3D12GraphicsCommandList* m_conversionList = nullptr;
    struct TargetRecording final : DlssNr::Submission::RecordingObserver
    {
        bool invalidated = false; // guarded by RecordingMutex
        ComPtr<ID3D12Resource> resource;
        void Invalidated(DlssNr::Submission::RecordingIdentity) noexcept override { invalidated = true; }
    };
    struct TargetSlot
    {
        ComPtr<ID3D12Resource> resource;
        DlssNr::Submission::GpuBatch::Tracker::Ticket ticket = DlssNr::Submission::GpuBatch::kInvalidTicket;
        DlssNr::Submission::GpuBatch::Tracker::CompletionReceiptPtr receipt;
        std::shared_ptr<TargetRecording> recording;
        bool inUavState = false, gpuInUavState = false;
    };
    std::vector<TargetSlot> m_depthTargets, m_debugTargets;

    TargetSlot& AcquireTarget(std::vector<TargetSlot>& targets, ID3D12GraphicsCommandList* list,
                              UINT width, UINT height, DXGI_FORMAT format, LPCWSTR name)
    {
        std::lock_guard recordingLock(DlssNr::Submission::RecordingMutex());
        auto& tracker = DlssNr::Submission::GpuBatch::Tracker::Instance();
        TargetSlot* selected = nullptr;
        for (auto& slot : targets)
        {
            if (slot.recording && !slot.recording->invalidated) continue;
            tracker.StatusOf(slot.ticket);
            if (slot.receipt && slot.receipt->outcome.load(std::memory_order_acquire) ==
                DlssNr::Submission::GpuBatch::Tracker::CompletionReceipt::Outcome::Outstanding)
                continue;
            if (slot.receipt)
            {
                if (slot.receipt->outcome.load(std::memory_order_acquire) ==
                    DlssNr::Submission::GpuBatch::Tracker::CompletionReceipt::Outcome::Completed)
                    slot.gpuInUavState = slot.inUavState;
                else
                    slot.inUavState = slot.gpuInUavState;
            }
            selected = &slot;
            break;
        }
        if (!selected)
        {
            if (targets.size() >= 256)
                throw std::runtime_error("FSRD target ring exhausted by outstanding recordings");
            targets.emplace_back();
            selected = &targets.back();
        }
        if (!selected->resource || selected->resource->GetDesc().Width != width ||
            selected->resource->GetDesc().Height != height)
        {
            selected->resource = CreateTexture2D(m_pDev, width, height, format, name, kSrvState);
            selected->inUavState = selected->gpuInUavState = false;
        }
        auto recording = std::make_shared<TargetRecording>();
        Microsoft::WRL::ComPtr<DlssNr::Submission::IRecordingResources> observer;
        if (FAILED(list->QueryInterface(IID_PPV_ARGS(&observer))) ||
            FAILED(observer->ObserveResources(recording)))
            throw std::runtime_error("FSRD target recording observer unavailable");
        selected->recording = std::move(recording);
        selected->recording->resource = selected->resource;
        selected->ticket = tracker.Record(list);
        selected->receipt = selected->ticket == DlssNr::Submission::GpuBatch::kInvalidTicket
            ? nullptr : tracker.ObserveCompletion(selected->ticket);
        if (!selected->receipt)
            throw std::runtime_error("FSRD target lifetime receipt unavailable");
        return *selected;
    }
    void ObserveHistoryWriter(ID3D12GraphicsCommandList* list,
                              const std::shared_ptr<HistoryGeneration>& generation)
    {
        auto use = std::make_shared<HistoryUse>();
        use->device = m_pDev;
        use->generation = generation;
        use->write = true;
        if (!use->Attach(list)) throw std::runtime_error("FSRD history recording observer unavailable");
        generation->writer = use->identity;
    }

    std::shared_ptr<HistoryGeneration> AcquireHistory()
    {
        std::lock_guard lock(DlssNr::Submission::RecordingMutex());
        std::shared_ptr<HistoryGeneration> selected;
        for (auto& generation : m_historyGenerations)
        {
            if (generation.use_count() != 1 || generation->uncertain || generation->contentUncertain) continue;
            const bool complete = std::all_of(generation->accesses.begin(), generation->accesses.end(),
                [](const auto& p) { const auto done = p.fence->GetCompletedValue();
                    return done != UINT64_MAX && done >= p.value; });
            if (complete) { selected = generation; break; }
        }
        if (!selected)
        {
            if (m_historyGenerations.size() >= 256)
                throw std::runtime_error("FSRD history pool exhausted by retained recordings");
            selected = std::make_shared<HistoryGeneration>();
            m_historyGenerations.push_back(selected);
        }
        if (!selected->decisions || selected->decisions->GetDesc().Width != m_maxWidth ||
            selected->decisions->GetDesc().Height != m_maxHeight)
        {
            selected->decisions = CreateTexture2D(m_pDev, m_maxWidth, m_maxHeight,
                DXGI_FORMAT_R16G16B16A16_FLOAT, L"FSR_Handover_Decisions", kSrvState);
            selected->metadata = CreateTexture2D(m_pDev, m_maxWidth, m_maxHeight,
                DXGI_FORMAT_R32G32B32A32_UINT, L"FSR_Handover_Metadata", kSrvState);
        }
        selected->accesses.clear();
        selected->writer = {};
        selected->admittedEpoch = 0;
        selected->reservedAccesses = 0;
        selected->produced = selected->discarded = selected->contentUncertain = false;
        return selected;
    }


    ID3D12Resource* PrepareUpscalerDepthInput(ID3D12GraphicsCommandList* cmdList,
                                             const UpscalerDepthEncode& encode)
    {
        m_upscalerDepthValid = false;
        if (!cmdList || cmdList != m_conversionList || !m_canonicalDepthValid ||
            !std::isfinite(encode.nearPlane) || encode.nearPlane <= 0.0f ||
            !std::isfinite(encode.farPlane) || encode.farPlane <= encode.nearPlane ||
            (encode.infiniteFar && encode.farPlane != (std::numeric_limits<float>::max)()))
            throw std::runtime_error("Invalid current canonical depth or final SR camera terms");
        m_upscalerDepthShader.Reserve(1);
        auto& target = AcquireTarget(m_depthTargets, cmdList, m_maxWidth, m_maxHeight,
                                     DXGI_FORMAT_R32_FLOAT, L"FSR_UpscalerDepth_Output");
        m_upscalerDepthOutput = target.resource;
        const UpscalerDepth::Constants constants {
            .RenderSize = m_upscalerDepthRenderSize,
            .NearPlane = encode.nearPlane,
            .FarPlane = encode.farPlane,
            .Flags = (encode.infiniteFar ? uint32_t(UpscalerDepth::Flags::InfiniteFar) : 0u) |
                     (encode.invertedOutput ? uint32_t(UpscalerDepth::Flags::InvertedOutput) : 0u)
        };
        UpscalerDepth::Input inputs { .Resources = { .InLinearDepth = m_canonicalDepth.Get() } };
        UpscalerDepth::Output outputs { .Resources = { .OutEncodedDepth = target.resource.Get() } };
        m_upscalerDepthShader.Dispatch(cmdList, GetAsByteSpan(constants), inputs.AsArray, outputs.AsArray,
                                       { constants.RenderSize.x, constants.RenderSize.y });
        m_upscalerDepthValid = true;
        return target.resource.Get();
    }
    XMFLOAT4 m_historyInputSettings {};
    uint32_t m_historyConversionFlags = 0;
    ComPtr<ID3D12Resource> m_outputBuffer1;
    ComPtr<ID3D12Resource> m_outputBuffer2;
    // RR direct-specular output (denoised unmodulated specular) and the trust ping-pong.
    FSRDStageTimings* m_stageTimings = nullptr;
    FSRDRuntimeSnapshot* m_runtime = nullptr;
    bool m_extraDiffuse = false, m_extraSpecular = false, m_albedoRecovery = false, m_diffuseCopy = false;
    ComPtr<ID3D12Resource> m_directSpecularOutput;
    ComPtr<ID3D12Resource> m_indirectDiffuseOutput;
    std::array<ComPtr<ID3D12Resource>, 2> m_albedoTrust;
    UINT m_albedoTrustResult = 0;

    ComPtr<ID3D12Resource> m_ambientOcclusionOutput;
    ComPtr<ID3D12Resource> m_specularOcclusionOutput;
    ComPtr<ID3D12Resource> m_debugViewOutput;
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
    static constexpr UINT kInputProbeInterval = 60; // conversions between recordings
    ComPtr<ID3D12Resource> m_inputProbeReadback[kInputProbeTargetCount];
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_inputProbeFootprint[kInputProbeTargetCount] = {};
    UINT m_inputProbeRowPitch[kInputProbeTargetCount] = {};
    XMFLOAT4 m_inputProbeMotionTransform = {};
    UINT m_inputProbeWidth = 0;
    UINT m_inputProbeHeight = 0;
    UINT m_inputProbeCountdown = 1; // record on the first conversion, then every interval
    // A CPU frame delay or a value copied into a readback heap is not a GPU fence.
    // The probe keeps the GpuBatch ticket plus a completion receipt attached
    // immediately after Record (before the batch can execute or the entry reap).
    // Polling promotes the fence result with StatusOf and then reads the receipt:
    // Completed only after the latest execution's fence completed (a replay re-arms
    // the receipt to Outstanding until its fence finishes), Discarded only when the
    // recording was provably discarded before submission. The receipt keeps its
    // terminal value after the tracker's entry is reaped.
    DlssNr::Submission::GpuBatch::Tracker::Ticket m_inputProbeTicket =
        DlssNr::Submission::GpuBatch::kInvalidTicket;
    DlssNr::Submission::GpuBatch::Tracker::CompletionReceiptPtr m_inputProbeReceipt;
    struct ProbeRecording final : DlssNr::Submission::RecordingObserver
    {
        std::array<ComPtr<ID3D12Resource>, kInputProbeTargetCount> readbacks;
        void Invalidated(DlssNr::Submission::RecordingIdentity) noexcept override {}
    };

    void ReleaseInputProbe()
    {
        m_inputProbeTicket = DlssNr::Submission::GpuBatch::kInvalidTicket;
        m_inputProbeReceipt.reset();
        // A discarded recording never wrote these buffers. The next capture gets a
        // fresh allocation rather than trusting stale content.
        for (auto& buffer : m_inputProbeReadback) buffer.Reset();
    }

    void UpdateInputProbe(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc)
    {
        std::lock_guard recording(DlssNr::Submission::RecordingMutex());
        using Outcome = DlssNr::Submission::GpuBatch::Tracker::CompletionReceipt::Outcome;
        if (m_inputProbeReceipt)
        {
            // Promote the fence result first so a completed batch publishes into the
            // receipt even when no further queue work would observe it.
            DlssNr::Submission::GpuBatch::Tracker::Instance().StatusOf(m_inputProbeTicket);
            const auto outcome = m_inputProbeReceipt->outcome.load(std::memory_order_acquire);
            if (outcome == Outcome::Discarded)
                ReleaseInputProbe();
            else if (outcome == Outcome::Completed && (!desc.DiagnosticsEnabled || LogInputProbe()))
            {
                // A retained executable recording may replay. Its observer keeps the
                // old buffers; never overwrite them with a later diagnostic capture.
                ReleaseInputProbe();
            }
        }

        // Poll outstanding captures even after diagnostics are disabled.
        if (!desc.DiagnosticsEnabled || m_inputProbeReceipt)
            return;

        if (m_inputProbeCountdown > 0 && --m_inputProbeCountdown > 0)
            return;

        m_inputProbeWidth = static_cast<UINT>(desc.RenderSize.x);
        m_inputProbeHeight = static_cast<UINT>(desc.RenderSize.y);
        m_inputProbeMotionTransform = desc.MotionTransform;
        // Also back off after an allocation failure; diagnostics must not
        // retry an expensive failing allocation on every rendered frame.
        m_inputProbeCountdown = kInputProbeInterval;
        RecordInputProbe(cmdList);
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
        if (m_pDev == nullptr || cmdList == nullptr)
            return false;

        ID3D12Resource* sources[kInputProbeTargetCount] = {
            m_LinearDepth.Get(),
            m_out.Resources.Motion.Get(),
            m_out.Resources.Normals.Get(),
            m_out.Resources.Signals.IndirectSpecular.Get(),
            m_out.Resources.SpecAlbedo.Get(),
            m_out.Resources.SkipSignal.Get(),
            m_out.Resources.DiffAlbedo.Get()
        };

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
            m_pDev->GetCopyableFootprints(
                &srcDesc, 0, 1, 0, &m_inputProbeFootprint[i], nullptr, &rowBytes, &bufferBytes);
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
            if (m_inputProbeReadback[i] == nullptr ||
                m_inputProbeReadback[i]->GetDesc().Width < maxBufferBytes)
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
                if (FAILED(m_pDev->CreateCommittedResource(
                        &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
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

        // The probe's sources and readbacks are Impl-owned; the whole preprocessor
        // outlives outstanding batches through the feature's GpuBatch tickets. The
        // readiness receipt attaches to this exact command list's recording now,
        // before the batch can execute or the entry be reaped.
        auto& tracker = DlssNr::Submission::GpuBatch::Tracker::Instance();
        m_inputProbeTicket = tracker.Record(cmdList);
        m_inputProbeReceipt =
            m_inputProbeTicket == DlssNr::Submission::GpuBatch::kInvalidTicket
                ? nullptr
                : tracker.ObserveCompletion(m_inputProbeTicket);
        if (!m_inputProbeReceipt)
        {
            ReleaseInputProbe();
            LOG_ERROR("[RR_INPUT_PROBE] capture lifetime receipt unavailable");
            return false;
        }

        auto recording = std::make_shared<ProbeRecording>();
        for (UINT i = 0; i < kInputProbeTargetCount; ++i)
            recording->readbacks[i] = m_inputProbeReadback[i];
        Microsoft::WRL::ComPtr<DlssNr::Submission::IRecordingResources> observer;
        if (FAILED(cmdList->QueryInterface(IID_PPV_ARGS(&observer))) ||
            FAILED(observer->ObserveResources(recording)))
        {
            ReleaseInputProbe();
            LOG_ERROR("[RR_INPUT_PROBE] recording observer unavailable");
            return false;
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
            ((width + sampling.strideX - 1) / sampling.strideX) *
            ((height + sampling.strideY - 1) / sampling.strideY);
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

                const UINT bucket = magnitude < 0.1f ? 0u
                    : magnitude < 1.0f ? 1u
                    : magnitude < 10.0f ? 2u
                    : magnitude < 100.0f ? 3u
                    : magnitude < 1000.0f ? 4u : 5u;
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

        LOG_INFO(
            "[RR_INPUT_PROBE] linearDepth (R32_FLOAT, RR-facing) {}x{}: samples={}, min={:.4f}, max={:.4f}, "
            "mean|v|={:.4f}, p50|v|={:.4f}, p95|v|={:.4f}, negative={:.1f}%, exactZero={:.1f}%, nonFinite={}",
            sampleWidth, sampleHeight, UINT(magnitudes.size()), minValue, maxValue,
            sum / double(magnitudes.size()), PercentileOf(magnitudes, 0.50f),
            PercentileOf(magnitudes, 0.95f), float(negatives) * rcpCount,
            float(zeros) * rcpCount, nonFinite);

        const char* bucketNames[6] = {
            "<0.1 (sub-metre)", "0.1-1", "1-10", "10-100", "100-1000", ">=1000"
        };
        LOG_INFO("[RR_INPUT_PROBE] linearDepth |v| distribution: {}={:.1f}%, {}={:.1f}%, {}={:.1f}%, "
                 "{}={:.1f}%, {}={:.1f}%, {}={:.1f}%",
                 bucketNames[0], float(buckets[0]) * rcpCount,
                 bucketNames[1], float(buckets[1]) * rcpCount,
                 bucketNames[2], float(buckets[2]) * rcpCount,
                 bucketNames[3], float(buckets[3]) * rcpCount,
                 bucketNames[4], float(buckets[4]) * rcpCount,
                 bucketNames[5], float(buckets[5]) * rcpCount);
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
            std::isfinite(transformX) && std::isfinite(transformY) &&
            transformX != 0.0f && transformY != 0.0f;

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

        LOG_INFO(
            "[RR_INPUT_PROBE] motion (RGBA16F, RR-facing) {}x{}: samples={}, canonicalPixels p50={:.3f}, "
            "p95={:.3f}, max={:.3f}, exactZeroXY={:.1f}%, moving(>0.05px)={:.1f}%, |depthDelta| p50={:.6f}, "
            "p95={:.6f}, max={:.6f}, nonFinite={}",
            sampleWidth, sampleHeight, UINT(pixelMagnitudes.size()),
            PercentileOf(pixelMagnitudes, 0.50f), PercentileOf(pixelMagnitudes, 0.95f),
            pixelMagnitudes.back(), float(zeroXy) * rcpCount, float(activeXy) * rcpCount,
            PercentileOf(depthDeltas, 0.50f), PercentileOf(depthDeltas, 0.95f),
            depthDeltas.back(), nonFinite);

        // The converter multiplies the stored value by MotionTransform to reach UV. A
        // transform of 1/renderWidth means the stored value was assumed to be a
        // render-pixel delta; a stored magnitude near 1e-3 with that transform is the
        // signature of a title that supplies UV-space motion instead.
        if (!storedMagnitudes.empty())
        {
            LOG_INFO(
                "[RR_INPUT_PROBE] motion transform=(x={:.8f}, y={:.8f}) => impliedScale=({:.4f}, {:.4f}); "
                "stored-in-texture|v| p50={:.6f}, p95={:.6f}, max={:.6f}",
                transformX, transformY,
                transformX * float(sampleWidth), transformY * float(sampleHeight),
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

                const float length = std::sqrt(
                    normalX * normalX + normalY * normalY + normalZ * normalZ);
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

        LOG_INFO(
            "[RR_INPUT_PROBE] normals (R10G10B10A2, RR-facing) {}x{}: samples={}, |octDecode| p50={:.4f}, "
            "p95={:.4f}, max={:.4f}, degenerate(<0.5)={:.1f}%, decodedWorldY p50={:.4f}, p95={:.4f}, "
            "downward(Y<-0.9)={:.1f}%, horizontal(|Y|<0.1)={:.1f}%",
            sampleWidth, sampleHeight, UINT(decodedLengths.size()),
            PercentileOf(decodedLengths, 0.50f), PercentileOf(decodedLengths, 0.95f),
            decodedLengths.back(), float(degenerate) * rcpCount,
            PercentileOf(upComponents, 0.50f), PercentileOf(upComponents, 0.95f),
            float(pointingDown) * rcpCount, float(horizontal) * rcpCount);

        LOG_INFO(
            "[RR_INPUT_PROBE] normals channels: roughness mean={:.4f}, exactZero={:.1f}%, "
            "materialType 0={:.1f}%, >=1={:.1f}%",
            float(roughnessSum / double(decodedLengths.size())),
            float(exactZeroRoughness) * rcpCount,
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
        LOG_INFO(
            "[RR_INPUT_PROBE] specular signal input (RGBA16F, RR-facing) {}x{}: samples={}, avgLuma={:.6f}, "
            "maxLuma={:.6f}, dead(<1e-5)={:.1f}%, bright(>1e-2)={:.1f}%, alpha min={:.4f}, mean={:.4f}, "
            "max={:.4f}, negative={:.1f}%, FP16maxSentinel={:.1f}%, nonFinite={}",
            sampleWidth, sampleHeight, samples, lumaSum / double(samples), lumaMax,
            float(dead) * rcpCount, float(bright) * rcpCount,
            alphaMin, alphaSum / double(samples), alphaMax,
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
                 float(shareSum) * 100.0f / float(samples),
                 float(specularDominant) * 100.0f / float(samples), specularDominant, samples);
    }

    // Magnitude of a colour texture, which is all the skip signal needs: the question is how
    // much colour it carries and whether that quantity is what the visible noise tracks.
    void LogMagnitudeProbe(const char* name, const uint8_t* data, UINT rowPitch,
                           const ProbeSampling& sampling, bool halfPrecision)
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

        LOG_INFO("[RR_INPUT_PROBE] {}: samples={}, avgLuma={:.6f}, maxLuma={:.6f}, dead(<1e-5)={:.1f}%",
                 name, samples, lumaSum / double(samples), lumaMax,
                 float(dead) * 100.0f / float(samples));
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

        if (!m_inputProbeReceipt)
            return false;
        // Promote the fence result before reading the receipt.
        DlssNr::Submission::GpuBatch::Tracker::Instance().StatusOf(m_inputProbeTicket);
        if (m_inputProbeReceipt->outcome.load(std::memory_order_acquire) !=
            DlssNr::Submission::GpuBatch::Tracker::CompletionReceipt::Outcome::Completed)
            return false;

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
        LOG_INFO("[RR_INPUT_PROBE] reading back {}x{} render extent from the conversion",
                 m_inputProbeWidth, m_inputProbeHeight);

        LogLinearDepthProbe(
            static_cast<const uint8_t*>(mapped[0]), m_inputProbeRowPitch[0], sampling);
        LogMotionProbe(
            static_cast<const uint8_t*>(mapped[1]), m_inputProbeRowPitch[1], sampling);
        LogNormalsProbe(
            static_cast<const uint8_t*>(mapped[2]), m_inputProbeRowPitch[2], sampling);
        LogSignalProbe(
            static_cast<const uint8_t*>(mapped[3]), m_inputProbeRowPitch[3], sampling);
        LogSpecularShareProbe(
            static_cast<const uint8_t*>(mapped[4]), m_inputProbeRowPitch[4], sampling);
        LogMagnitudeProbe("skip signal (RR-facing)", static_cast<const uint8_t*>(mapped[5]),
                          m_inputProbeRowPitch[5], sampling, true);
        LogCrossingShareProbe(
            static_cast<const uint8_t*>(mapped[6]), m_inputProbeRowPitch[6], sampling);

        for (UINT i = 0; i < kInputProbeTargetCount; i++)
            m_inputProbeReadback[i]->Unmap(0, nullptr);

        return true;
    }

    void Initialize(
        std::span<const byte> blSeedByteCode, 
        std::span<const byte> blPyramidByteCode, 
        std::span<const byte> convByteCode, 
        std::span<const byte> compByteCode
    )
    {
        ScopedSkipHeapCapture skipHeapCapture {};

        LOG_DEBUG("Creating FSRD interop shaders...");

        m_floorSeedShader.Initialize(m_pDev, blSeedByteCode, sizeof(FloorSeed::Constants), 
            FloorSeed::Input::kCount, FloorSeed::Output::kCount, L"FSRD_FloorSeed_Constants", FloorSeed::kBackBufferCount);
        m_floorFilterShader.Initialize(m_pDev, blPyramidByteCode, sizeof(FloorFilter::Constants), 
            FloorFilter::Input::kCount, FloorFilter::Output::kCount, L"FSRD_FloorFilter_Constants", FloorFilter::kBackBufferCount);
        m_convShader.Initialize(m_pDev, convByteCode, sizeof(Conversion::Constants), 
            Conversion::Input::kCount, Conversion::Output::kCount, L"FSRD_Conv_Constants", Conversion::kBackBufferCount);
        m_compShader.Initialize(m_pDev, compByteCode, sizeof(Composition::Constants), 
            Composition::Input::kCount, Composition::kOutputCount, L"FSRD_Comp_Constants", Composition::kBackBufferCount);
        m_trustEvidenceShader.Initialize(m_pDev,
            { reinterpret_cast<const byte*>(FSRDAlbedoTrustEvidence_cso), sizeof(FSRDAlbedoTrustEvidence_cso) },
            sizeof(TrustEvidence::Constants), TrustEvidence::Input::kCount, TrustEvidence::Output::kCount,
            L"FSRD_TrustEvidence_Constants", TrustEvidence::kBackBufferCount);
        m_trustEvidenceShader.m_groupX = m_trustEvidenceShader.m_groupY = TrustEvidence::kThreadGroupSize;
        m_trustPropagateShader.Initialize(m_pDev,
            { reinterpret_cast<const byte*>(FSRDAlbedoTrustPropagate_cso), sizeof(FSRDAlbedoTrustPropagate_cso) },
            sizeof(TrustPropagate::Constants), TrustPropagate::Input::kCount, TrustPropagate::Output::kCount,
            L"FSRD_TrustPropagate_Constants", TrustPropagate::kBackBufferCount);
        m_upscalerDepthShader.Initialize(m_pDev,
            { reinterpret_cast<const byte*>(FSRDUpscalerDepth_cso), sizeof(FSRDUpscalerDepth_cso) },
            sizeof(UpscalerDepth::Constants), UpscalerDepth::Input::kCount, UpscalerDepth::Output::kCount,
            L"FSRD_UpscalerDepth_Constants", UpscalerDepth::kBackBufferCount);

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
        m_canonicalDepthValid = m_upscalerDepthValid = false;
        m_conversionList = nullptr;
        m_historyValid = m_historyPending = false;
        m_historyPrevious.reset();
        m_historyNext.reset();

        auto CreateTex = [&](DXGI_FORMAT fmt, LPCWSTR name, UINT mipLevels = 1)
        { 
            return CreateTexture2D(m_pDev, width, height, fmt, name, kSrvState, mipLevels);
        };

        auto& outResources = m_out.Resources;
        outResources.Motion = CreateTex(FSRDFormats::Motion, L"FSR_Conv_Motion");
        outResources.Normals = CreateTex(FSRDFormats::Normals, L"FSR_Conv_Normals");
        outResources.SpecAlbedo = CreateTex(FSRDFormats::SpecAlbedo, L"FSR_Conv_SpecAlbedo");
        outResources.DiffAlbedo = CreateTex(FSRDFormats::DiffAlbedo, L"FSR_Conv_DiffAlbedo");
        outResources.SkipSignal = CreateTex(FSRDFormats::SkipSignal, L"FSR_Conv_SkipSignal");
        outResources.DetailReference = CreateTex(FSRDFormats::DetailReference, L"FSR_Conv_DetailReference");
        const auto optional = [&](bool enabled, DXGI_FORMAT format, LPCWSTR name) {
            return CreateTexture2D(m_pDev, enabled ? width : 1u, enabled ? height : 1u, format, name, kSrvState);
        };
        outResources.DirectSpecular = optional(m_albedoRecovery, FSRDFormats::DirectSpecular, L"FSR_Conv_DirectSpecular");
        outResources.IndirectDiffuse = optional(m_diffuseCopy, FSRDFormats::DirectSpecular, L"FSR_Conv_IndirectDiffuse");
        m_directSpecularOutput = optional(m_extraSpecular, FSRDFormats::DirectSpecular, L"FSR_RR_DirectSpecular_Output");
        m_indirectDiffuseOutput = optional(m_extraDiffuse, FSRDFormats::DirectSpecular, L"FSR_RR_IndirectDiffuse_Output");
        m_albedoTrust[0] = optional(m_albedoRecovery, FSRDFormats::AlbedoTrust, L"FSR_AlbedoTrust_0");
        m_albedoTrust[1] = optional(m_albedoRecovery, FSRDFormats::AlbedoTrust, L"FSR_AlbedoTrust_1");
        m_albedoTrustResult = 0;
        m_LinearDepth = CreateTex(FSRDFormats::LinearDepth, L"FSR_Conv_LinearDepth");
        m_canonicalDepth = CreateTex(DXGI_FORMAT_R32_FLOAT, L"FSR_Conv_UnclampedViewDepth");
        m_outputBuffer1 = CreateTex(FSRDFormats::OutputBuffer1, L"FSR_Conv_OutputBuffer1");
        m_outputBuffer2 = CreateTex(FSRDFormats::OutputBuffer2, L"FSR_Conv_OutputBuffer2");
        m_ambientOcclusionOutput =
            CreateTex(FSRDFormats::AmbientOcclusion, L"FSR_RR_AmbientOcclusion_Output");
        m_specularOcclusionOutput =
            CreateTex(FSRDFormats::SpecularOcclusion, L"FSR_RR_SpecularOcclusion_Output");

        m_floorReference = CreateTex(FSRDFormats::DetailReference, L"FSR_Floor_Reference");
        m_compositionOutput = CreateTex(DXGI_FORMAT_R16G16B16A16_FLOAT, L"FSR_Composition_Output");
        m_upscalerDepthValid = false;
        m_smoothFloor = nullptr;
        m_radianceOutputsInUavState = false;
        m_ambientOcclusionOutputInUavState = false;
        m_specularOcclusionOutputInUavState = false;

        outResources.Signals =
        {
            .IndirectSpecular = CreateTex(FSRDFormats::IndirectSpecular, L"FSR_Conv_IndirectSpecular"),
            .DirectDiffuse = CreateTex(FSRDFormats::DirectDiffuse, L"FSR_Conv_DirectDiffuse")
        };

        // Every allocation succeeded, so the new size is now the real state.
        m_maxWidth = width;
        m_maxHeight = height;
    }

    void DispatchFloorSeed(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc)
    {
        FloorSeed::Constants constants = {
            .InvProjMatrix = desc.InvProjMatrix,
            .RenderSize = desc.RenderSize,
            .NearPlane = desc.NearPlane,
            .FarPlane = desc.FarPlane,
            .Flags = ((desc.Flags & uint32_t(ConvFlags::IsDepthLinear)) ? uint32_t(FloorSeed::Flags::LinearDepth) : 0u) |
                     ((desc.Flags & uint32_t(ConvFlags::RightHanded)) ? uint32_t(FloorSeed::Flags::NegativeViewDepth) : 0u) |
                     ((desc.Flags & uint32_t(ConvFlags::TitleLinearDepth)) && desc.Resources.InTitleLinearDepth
                          ? uint32_t(FloorSeed::Flags::TitleLinearDepth) : 0u),
            .CurrentJitter = {desc.JitterOffsets.x, desc.JitterOffsets.y},
            .InputBase = desc.FloorSourceBase,
            .NormalBase = {desc.InputBase1.x, desc.InputBase1.y},
            .TitleDepthBase = desc.TitleLinearDepthBase,
            .AlbedoBase = {desc.InputBase2.z, desc.InputBase2.w},
            .FloorEnabled = desc.FloorEnabled ? 1u : 0u
        };
        FloorSeed::Input in = {.Resources = {
            .InColor = desc.Resources.InColor,
            .InNormals = desc.Resources.InNormals,
            .InDepth = desc.Resources.InDepth,
            .InTitleLinearDepth = desc.Resources.InTitleLinearDepth,
            .InDiffAlbedo = desc.Resources.InDiffAlbedo
        }};
        FloorSeed::Output out = {.Resources = {
            .OutColor = m_outputBuffer2.Get(),
            .OutLinearDepth = m_LinearDepth.Get(),
            .OutDepthGradient = m_out.Resources.Motion.Get(),
            .OutDetailReference = m_floorReference.Get(),
            .OutCanonicalDepth = m_canonicalDepth.Get()
        }};
        m_floorSeedShader.Dispatch(cmdList, GetAsByteSpan(constants), in.AsArray, out.AsArray,
                                  {desc.RenderSize.x, desc.RenderSize.y});
        m_smoothFloor = m_outputBuffer2.Get();
    }

    void DispatchFloorFilter(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc)
    {
        if (!desc.FloorEnabled) return;
        static constexpr std::array<int, FloorFilter::kPasses> fullSteps {1, 2, 4, 8, 16};
        static constexpr std::array<int, 3> fastSteps {1, 2, 16};
        const std::span<const int> steps = desc.FloorFastMode
            ? std::span<const int>(fastSteps) : std::span<const int>(fullSteps);
        for (int step : steps)
        {
            FloorFilter::Constants constants = {
                .DstTexSize = desc.RenderSize,
                .StepSize = step,
                .AlbedoBase = {desc.InputBase2.z, desc.InputBase2.w}
            };
            FloorFilter::Input in = {.Resources = {
                .InColor = m_smoothFloor,
                .InLinearDepth = m_LinearDepth.Get(),
                .InDepthGradient = m_out.Resources.Motion.Get(),
                .InDiffAlbedo = desc.Resources.InDiffAlbedo
            }};
            FloorFilter::Output out = {.Resources = {.OutColor = m_outputBuffer1.Get()}};
            m_floorFilterShader.Dispatch(cmdList, GetAsByteSpan(constants), in.AsArray, out.AsArray,
                                        {desc.RenderSize.x, desc.RenderSize.y});
            std::swap(m_outputBuffer1, m_outputBuffer2);
            m_smoothFloor = m_outputBuffer2.Get();
        }
    }

    bool EnsureAdditiveConversionPipeline()
    {
        if (m_additiveConvPso)
            return true;
        if (m_additiveConvPsoFailed)
            return false;

        ScopedSkipHeapCapture skipHeapCapture {};
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = m_convShader.m_rootSig.Get();
        psoDesc.CS = { FSRDInputConvAdditive_cso, sizeof(FSRDInputConvAdditive_cso) };
        ComPtr<ID3D12PipelineState> additivePso;
        const HRESULT result = m_pDev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&additivePso));
        if (FAILED(result))
        {
            m_additiveConvPsoFailed = true;
            LOG_ERROR("FSRD additive conversion pipeline failed to initialize (HRESULT: {}); disable Additive Light Split to use the original pipeline",
                      static_cast<uint32_t>(result));
            return false;
        }
        m_additiveConvPso = std::move(additivePso);
        return true;
    }

    void DispatchPackingShader(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc,
                               ID3D12PipelineState* conversionPipeline)
    {
        const XMFLOAT2 dispatchSize = { desc.RenderSize.x, desc.RenderSize.y };

        // Prepare inputs for packing and format conversion
        Conversion::Input in = { .Resources =
        {
            .InColor = desc.Resources.InColor,
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
            .InResponsivityMask = desc.Resources.InResponsivityMask,
            .InDetailReference = m_floorReference.Get()
        }};

        uint32_t packFlags = desc.Flags | uint32_t(ConvFlags::IsDepthLinear);
        // A null SRV reads as zero, so the shader is safe either way, but the flag keeps the
        // "no mask provided" case explicit and visible in the debug views.
        if (desc.Resources.InBiasMask != nullptr)
            packFlags |= uint32_t(ConvFlags::HasBiasMask);
        if (desc.FloorEnabled)
            packFlags |= uint32_t(ConvFlags::FloorEnabled);
        if (desc.Resources.InSpecularRayDirectionHitDistance &&
            desc.SpecularHitDistanceFromCombinedAlpha)
        {
            packFlags |= uint32_t(ConvFlags::HasCombinedSpecHitDistance);
        }

        Conversion::Constants packConstants =
        {
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
            .InputBase4 = {
                desc.InputBase4.x, desc.InputBase4.y,
                desc.DiffuseHitDistanceBase.x ? desc.DiffuseHitDistanceBase.x : desc.InputBase4.z,
                desc.DiffuseHitDistanceBase.y ? desc.DiffuseHitDistanceBase.y : desc.InputBase4.w
            },
            .InputBase5 = {
                desc.TitleLinearDepthBase.x, desc.TitleLinearDepthBase.y,
                desc.SpecularHitDistanceBase.x, desc.SpecularHitDistanceBase.y
            },
            .NearPlane = desc.NearPlane,
            .FarPlane = desc.FarPlane,
            .FloorDetailPreservation = desc.FloorDetailPreservation,
            // The packing shader never sees the game's hardware depth. FloorSeed
            // has already converted it to signed linear view-space depth.
            .Flags = packFlags,
            .InspectorChannel = desc.InspectorChannel,
            .InspectorScale = desc.InspectorScale,
            .DebugDepthMax = desc.DebugDepthMax,
            // Only honour a mode the caller actually bound a resource for.
            .DiffuseHitDistanceMode = desc.Resources.InDiffuseHitDistance != nullptr
                ? desc.DiffuseHitDistanceMode
                : 0u,
            .ResponsivityTrustThreshold = desc.ResponsivityTrustThreshold,
            .ResponsivityInvert = desc.ResponsivityInvert ? 1u : 0u,
            .BiasMaskStrength = desc.BiasMaskStrength,
            .DemodDivisorFloor = desc.DemodDivisorFloor,
            .SpecularAlbedoDemodulation = desc.SpecularAlbedoDemodulation,
            .DiffuseAlbedoModulation = desc.DiffuseAlbedoModulation,
            .RecoveryMask = desc.RecoveryMask,
            .AdditiveLightSplit = desc.AdditiveLightSplit,
        };

        CaptureAdditive(cmdList, packConstants, in.AsArray);
        const std::span<const byte> convCBData((const byte*) &packConstants, sizeof(packConstants));
        m_convShader.Dispatch(cmdList, convCBData, in.AsArray, m_out.AsRawArray, dispatchSize, true,
                              conversionPipeline);
    }

    bool DispatchConversion(ID3D12GraphicsCommandList* cmdList, const ConversionDesc& desc)
    {
        std::lock_guard recordingLock(DlssNr::Submission::RecordingMutex());
        m_canonicalDepthValid = m_upscalerDepthValid = false;
        m_conversionList = nullptr;
        if (m_runtime) m_runtime->Begin(FSRDRuntimeSnapshot::Floor);
        if (!cmdList || !m_maxWidth || !std::isfinite(desc.RenderSize.x) ||
            !std::isfinite(desc.RenderSize.y) || desc.RenderSize.x <= 0 || desc.RenderSize.y <= 0 ||
            desc.RenderSize.x > m_maxWidth || desc.RenderSize.y > m_maxHeight)
            return false;
        if (!RecordedShader::Supported(cmdList)) return false;
        m_upscalerDepthRenderSize = desc.RenderSize;

        const bool useAdditivePipeline = std::isfinite(desc.AdditiveLightSplit) && desc.AdditiveLightSplit > 0.0f;
        // Resolve the optional PSO before recording any Floor work or barriers.
        if (useAdditivePipeline && !EnsureAdditiveConversionPipeline())
            return false;
        ID3D12PipelineState* conversionPipeline = useAdditivePipeline
            ? m_additiveConvPso.Get() : m_convShader.m_pso.Get();
        // Reserve before any barriers, timing commands, or ping-pong swaps.
        m_floorSeedShader.Reserve(1);
        m_floorFilterShader.Reserve(desc.FloorEnabled ? (desc.FloorFastMode ? 3u : FloorFilter::kPasses) : 0u);
        m_convShader.Reserve(1);

        const std::array<XMUINT4,7> sourceBases { desc.FloorSourceBase,desc.InputBase0,
            desc.InputBase1,desc.InputBase2,desc.InputBase3,
            XMUINT4{desc.InputBase4.x,desc.InputBase4.y,
                    desc.DiffuseHitDistanceBase.x ? desc.DiffuseHitDistanceBase.x : desc.InputBase4.z,
                    desc.DiffuseHitDistanceBase.y ? desc.DiffuseHitDistanceBase.y : desc.InputBase4.w},
            XMUINT4{desc.TitleLinearDepthBase.x,desc.TitleLinearDepthBase.y,
                    desc.SpecularHitDistanceBase.x,desc.SpecularHitDistanceBase.y} };
        const XMFLOAT4 inputSettings { desc.BiasMaskStrength,desc.ResponsivityTrustThreshold,
            desc.ResponsivityInvert ? 1.0f : 0.0f,desc.DemodDivisorFloor };
        if (!desc.MotionHistoryValid || !desc.FloorEnabled ||
            desc.Flags != m_historyConversionFlags ||
            memcmp(&inputSettings,&m_historyInputSettings,sizeof(inputSettings)) != 0 ||
            memcmp(sourceBases.data(),m_historySourceBases.data(),sizeof(sourceBases)) != 0 ||
            memcmp(&desc.RenderSize,&m_historyRenderSize,sizeof(desc.RenderSize)) != 0)
            m_historyValid = false;
        m_historySourceBases=sourceBases;
        m_historyRenderSize=desc.RenderSize;
        m_historyConversionFlags=desc.Flags;
        m_historyInputSettings=inputSettings;
        m_motionHistoryValid=desc.MotionHistoryValid;
        m_historyJitterDelta={desc.JitterOffsets.z-desc.JitterOffsets.x,
                              desc.JitterOffsets.w-desc.JitterOffsets.y};

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
        if ((desc.Flags & uint32_t(ConvFlags::TitleLinearDepth)) != 0 &&
            desc.Resources.InTitleLinearDepth != nullptr)
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
        if (m_rrLinearDepth != nullptr &&
            m_rrLinearDepthDeclaredState != static_cast<uint32_t>(kSrvState))
        {
            AddBarrier(cmdList, m_rrLinearDepth,
                       static_cast<D3D12_RESOURCE_STATES>(m_rrLinearDepthDeclaredState), kSrvState);
            m_rrLinearDepthForwarded = true;
        }

        TransitionDenoiserOutputsToRead(cmdList);

        // Filtered raster lighting estimate
        {
            FSRDStageTimings::Scope timing(m_stageTimings, FSRDStageTimings::Floor);
            DispatchFloorSeed(cmdList, desc);
            DispatchFloorFilter(cmdList, desc);
        }
        if (m_runtime)
        {
            m_runtime->steps[FSRDRuntimeSnapshot::Floor] = desc.FloorEnabled
                ? FSRDRuntimeSnapshot::Passed : FSRDRuntimeSnapshot::Disabled;
            m_runtime->Begin(FSRDRuntimeSnapshot::Conversion);
        }

        // DLSS-RR to FSR-RR conversion
        {
            FSRDStageTimings::Scope timing(m_stageTimings, FSRDStageTimings::Conversion);
            DispatchPackingShader(cmdList, desc, conversionPipeline);
        }
        if (m_runtime) m_runtime->Complete(FSRDRuntimeSnapshot::Conversion);
        // Published only once every conversion stage and handoff barrier succeeded.

        // Diagnostic: read back the RR-facing linear depth, motion and normals while
        // their state is still the one this code set. The denoiser dispatch below
        // records FFX's own barriers over these resources, so this must stay here.
        UpdateInputProbe(cmdList, desc);

        // Transition output buffers to UAV after last composition pass or first init.
        // The denoiser will be writing to these.
        {
            AddBarrier(cmdList, m_outputBuffer1.Get(), kSrvState, kUavState);
            AddBarrier(cmdList, m_outputBuffer2.Get(), kSrvState, kUavState);
            AddBarrier(cmdList, m_directSpecularOutput.Get(), kSrvState, kUavState);
            AddBarrier(cmdList, m_indirectDiffuseOutput.Get(), kSrvState, kUavState);
            AddBarrier(cmdList, m_ambientOcclusionOutput.Get(), kSrvState, kUavState);
            AddBarrier(cmdList, m_specularOcclusionOutput.Get(), kSrvState, kUavState);
            m_radianceOutputsInUavState = true;
            m_ambientOcclusionOutputInUavState = true;
            m_specularOcclusionOutputInUavState = true;
        }
        m_canonicalDepthValid = true;
        m_conversionList = cmdList;
        return true;
    }

    bool EnsureSplitCompositionPipelines()
    {
        if (m_tileLightCompPso && m_tileAnchorCompPso) return true;
        if (m_splitCompPsoFailed) return false;
        ScopedSkipHeapCapture skipHeapCapture {};
        // Ensure BOTH before recording either dispatch. Failure keeps the original
        // single generic dispatch, so no partial output/history can be published.
        const auto create = [&](D3D12_SHADER_BYTECODE code, ComPtr<ID3D12PipelineState>& result) {
            if (result) return true;
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = m_compShader.m_rootSig.Get();
            desc.CS = code;
            const HRESULT status = m_pDev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&result));
            if (SUCCEEDED(status)) return true;
            result.Reset();
            LOG_WARN("FSRD optional split composition pipeline failed (HRESULT: {}); using generic composition",
                     static_cast<uint32_t>(status));
            return false;
        };
        if (!create({ FSRDOutputCompTileLight_cso, sizeof(FSRDOutputCompTileLight_cso) }, m_tileLightCompPso) ||
            !create({ FSRDOutputCompTileAnchor_cso, sizeof(FSRDOutputCompTileAnchor_cso) }, m_tileAnchorCompPso))
        {
            m_splitCompPsoFailed = true;
            return false;
        }
        return true;
    }

    ID3D12PipelineState* CompositionPipeline(const CompositionDesc& desc)
    {
        const auto variant = Composition::ChoosePipeline(desc.Flags, desc.FloorDetailPreservation,
                                                         desc.RecoveryMask, desc.SpatialTemporalMask);
        if (variant == Composition::PipelineVariant::Generic) return m_compShader.m_pso.Get();
        const bool light = variant == Composition::PipelineVariant::Light;
        auto& pipeline = light ? m_lightCompPso : m_noRecoveryCompPso;
        auto& failed = light ? m_lightCompPsoFailed : m_noRecoveryCompPsoFailed;
        if (!pipeline && !failed)
        {
            ScopedSkipHeapCapture skipHeapCapture {};
            D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
            psoDesc.pRootSignature = m_compShader.m_rootSig.Get();
            psoDesc.CS = light
                ? D3D12_SHADER_BYTECODE { FSRDOutputCompLight_cso, sizeof(FSRDOutputCompLight_cso) }
                : D3D12_SHADER_BYTECODE { FSRDOutputCompNoRecovery_cso, sizeof(FSRDOutputCompNoRecovery_cso) };
            const HRESULT result = m_pDev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pipeline));
            if (FAILED(result))
            {
                failed = true;
                LOG_WARN("FSRD optional composition pipeline failed (HRESULT: {}); using generic composition",
                         static_cast<uint32_t>(result));
            }
        }
        return pipeline ? pipeline.Get() : m_compShader.m_pso.Get();
    }

    void DispatchComposition(ID3D12GraphicsCommandList* cmdList, const CompositionDesc& desc)
    {
        std::lock_guard recordingLock(DlssNr::Submission::RecordingMutex());
        if (m_runtime) m_runtime->Begin(FSRDRuntimeSnapshot::Composition);
        if (!RecordedShader::Supported(cmdList) || !m_maxWidth)
            throw std::runtime_error("Composition requires a resource-owning compute recording and allocated resources");

        m_historyPending=false;
        const bool writeHistory=desc.FloorDetailPreservation>0 && desc.RecoveryMask != 0 &&
            (desc.Flags & uint32_t(CompFlags::Debug))==0;
        // Every retained recording freezes its predecessor. Reuse requires all
        // producer/reader recordings invalidated and all queue fences completed.
        m_historyNext.reset();
        const bool readHistory = writeHistory && m_historyValid && m_motionHistoryValid &&
            m_historyPrevious && m_historyPrevious->CanRead(cmdList);
        std::shared_ptr<CompositionRead> reader;
        if (writeHistory) m_historyNext = AcquireHistory();
        if (readHistory)
        {
            reader = std::make_shared<CompositionRead>();
            reader->device = m_pDev;
            reader->generation = m_historyPrevious;
        }
        const bool split = Composition::CanSplitTiles(desc.Flags, desc.FloorDetailPreservation,
            desc.RecoveryMask, desc.SpatialTemporalMask) && EnsureSplitCompositionPipelines();
        ID3D12PipelineState* const compositionPipeline = split ? nullptr : CompositionPipeline(desc);
        m_compShader.Reserve(split ? 2u : 1u);
        if (desc.UnsupportedAlbedoRecovery > 0.0f)
        {
            m_trustEvidenceShader.Reserve(1);
            m_trustPropagateShader.Reserve(TrustPropagate::kPasses);
        }
        auto& outResources = m_out.Resources;
        Composition::Input inputs = {};
        Composition::Constants constants = 
        {
            .DstTexSize = desc.DstTexSize,
            .Flags = UINT(desc.Flags),
            .DetailPreservation = desc.FloorDetailPreservation,
            .RecoveryMask = desc.RecoveryMask,
            .FloorHandoverAnchorClamp = desc.FloorHandoverAnchorClamp,
            .SourceUvScale = {1.0f, 1.0f},
            .SourceUvOffset = {},
            .FloorHandoverCorrelationMix = desc.FloorHandoverCorrelationMix,
            // Do not arm a speculative read until every pass and hook exists.
            .HistoryValid = 0,
            .HistoryJitterDelta = m_historyJitterDelta,
            .WriteHistory = writeHistory ? 1u : 0u,
            .SpecularAlbedoDemodulation = desc.SpecularAlbedoDemodulation,
            .DiffuseAlbedoModulation = desc.DiffuseAlbedoModulation,
            .SpatialTemporalMask = desc.SpatialTemporalMask,
            .LumaRecovery = desc.LumaRecovery,
            .ChromaRecovery = desc.ChromaRecovery,
            .UnsupportedAlbedoRecovery = desc.UnsupportedAlbedoRecovery,
            .DemodDivisorFloor = desc.DemodDivisorFloor
        };

        // Transition denoiser output buffers to SRV for composition.
        TransitionDenoiserOutputsToRead(cmdList);

        // A signal excluded from the denoiser chain (single-signal denoise mode) is
        // never written this frame, and its ping-pong buffer is shared with the floor
        // passes - so it still holds that floor's intermediate image. Multiplied by
        // the albedo it would add floor light to the disabled channel instead of the
        // signal. The packed raw signal is the demodulated radiance the denoiser
        // would otherwise have consumed, so it is the energy-preserving passthrough.
        ID3D12Resource* const specularRadiance =
            (desc.Flags & (uint32_t) CompFlags::SpecularSignalDisabled) != 0
                ? outResources.Signals.IndirectSpecular.Get()
                : m_outputBuffer1.Get();
        ID3D12Resource* const diffuseRadiance =
            (desc.Flags & (uint32_t) CompFlags::DiffuseSignalDisabled) != 0
                ? outResources.Signals.DirectDiffuse.Get()
                : m_outputBuffer2.Get();

        if (desc.UnsupportedAlbedoRecovery > 0.0f)
        {
            if (m_runtime) m_runtime->Begin(FSRDRuntimeSnapshot::AlbedoRecovery);
            DispatchAlbedoTrust(cmdList, desc, diffuseRadiance);
            if (m_runtime) m_runtime->Complete(FSRDRuntimeSnapshot::AlbedoRecovery);
        }
        else if (m_runtime)
            m_runtime->steps[FSRDRuntimeSnapshot::AlbedoRecovery] = FSRDRuntimeSnapshot::Disabled;
        if (m_runtime) m_runtime->Begin(FSRDRuntimeSnapshot::Composition);

        inputs.Resources =
        {
            .InIndirectSpecular = specularRadiance,
            .InSpecularAlbedo = outResources.SpecAlbedo.Get(),
            .InDirectDiffuse = diffuseRadiance,
            .InDiffuseAlbedo = outResources.DiffAlbedo.Get(),
            .InSkipSignal = outResources.SkipSignal.Get(),
            .InNormals = outResources.Normals.Get(),
            .InDetailReference = outResources.DetailReference.Get(),
            .InLinearDepth = m_LinearDepth.Get(),
            .InMotion = outResources.Motion.Get(),
            .InDecisionHistory = nullptr,
            .InHistoryMetadata = nullptr,
            // Always bound; read only while UnsupportedAlbedoRecovery is nonzero.
            .InDirectSpecularDenoised = m_directSpecularOutput.Get(),
            .InDirectSpecularSignal = outResources.DirectSpecular.Get(),
            .InAlbedoTrust = m_albedoTrust[m_albedoTrustResult].Get(),
            .InIndirectDiffuseDenoised = m_indirectDiffuseOutput.Get(),
            .InIndirectSpecularSignal = outResources.Signals.IndirectSpecular.Get(),
            .InDirectDiffuseSignal = outResources.Signals.DirectDiffuse.Get(),
        };

        Composition::Output outputs { .Resources = {
            .OutColor=m_compositionOutput.Get(),
            .OutDecisionHistory=writeHistory ? m_historyNext->decisions.Get() : nullptr,
            .OutHistoryMetadata=writeHistory ? m_historyNext->metadata.Get() : nullptr } };
        const std::span<const byte> cbData((const byte*) &constants, sizeof(constants));
        const XMFLOAT2 dstDim = { constants.DstTexSize.x, constants.DstTexSize.y };

        {
            FSRDStageTimings::Scope timing(m_stageTimings, FSRDStageTimings::Composition);
            if (split)
            {
                // Each call creates its own retained dispatch lease/CBV/descriptors.
                // Both use unchanged SRVs and the same non-aliased history WRITE UAVs.
                // autoBarrierOutput restores SRV state after the first pass before the
                // second transitions it back to UAV; it also orders the disjoint writes.
                auto light = m_compShader.Dispatch(cmdList, cbData, inputs.AsArray, outputs.AsArray, dstDim, true,
                                                   m_tileLightCompPso.Get(), m_historyNext);
                auto anchor = m_compShader.Dispatch(cmdList, cbData, inputs.AsArray, outputs.AsArray, dstDim, true,
                                                    m_tileAnchorCompPso.Get(), m_historyNext);
                if (reader) { reader->slots = { std::move(light), std::move(anchor) }; reader->count = 2; }
            }
            else
            {
                auto slot = m_compShader.Dispatch(cmdList, cbData, inputs.AsArray, outputs.AsArray, dstDim, true,
                                                  compositionPipeline, m_historyNext);
                if (reader) { reader->slots[0] = std::move(slot); reader->count = 1; }
            }
        }
        // A partial multipass recording never acquires a writer identity. Slot
        // owners already retain its referenced allocations on every failure path.
        if (reader) reader->selection = HistoryUse::Selection::NoHistory;
        if (reader && !reader->Attach(cmdList))
            throw std::runtime_error("FSRD history reader finalizer unavailable");
        if (writeHistory) ObserveHistoryWriter(cmdList, m_historyNext);
        if (reader) reader->EnableHistory();
        m_historyPending=writeHistory;
        if (m_runtime) m_runtime->Complete(FSRDRuntimeSnapshot::Composition);
    }

    // Unsupported-albedo evidence from RR's unmodulated specular output, then six
    // same-surface a-trous steps that spread it over continuous surfaces. Requires the
    // denoiser outputs in SRV state; leaves the result in m_albedoTrust[m_albedoTrustResult].
    void DispatchAlbedoTrust(ID3D12GraphicsCommandList* cmdList, const CompositionDesc& desc,
                             ID3D12Resource* diffuseRadiance)
    {
        FSRDStageTimings::Scope timing(m_stageTimings, FSRDStageTimings::AlbedoRecovery);
        auto& outResources = m_out.Resources;
        const XMFLOAT2 dim = { desc.DstTexSize.x, desc.DstTexSize.y };
        TrustEvidence::Constants evidenceConstants = {
            .DstTexSize = desc.DstTexSize,
            .StepSize = 0,
            .Flags = (desc.Flags & uint32_t(CompFlags::ExtraDiffuse)) != 0 ? 1u : 0u,
            .DemodDivisorFloor = desc.DemodDivisorFloor
        };
        TrustEvidence::Input evidenceIn = { .Resources = {
            .InDirectSpecularDenoised = m_directSpecularOutput.Get(),
            .InDirectDiffuse = diffuseRadiance,
            .InSpecularAlbedo = outResources.SpecAlbedo.Get(),
            .InDiffuseAlbedo = outResources.DiffAlbedo.Get(),
            .InSkipSignal = outResources.SkipSignal.Get(),
            .InLinearDepth = m_LinearDepth.Get(),
            .InNormals = outResources.Normals.Get(),
            .InDirectSpecularSignal = outResources.DirectSpecular.Get(),
            .InIndirectDiffuseDenoised = m_indirectDiffuseOutput.Get(),
            .InIndirectSpecularSignal = outResources.Signals.IndirectSpecular.Get(),
            .InDirectDiffuseSignal = outResources.Signals.DirectDiffuse.Get()
        }};
        TrustEvidence::Output evidenceOut = { .Resources = { .OutAlbedoTrust = m_albedoTrust[0].Get() } };
        m_trustEvidenceShader.Dispatch(cmdList, GetAsByteSpan(evidenceConstants), evidenceIn.AsArray,
                                       evidenceOut.AsArray, dim);

        UINT read = 0;
        for (UINT pass = 0; pass < TrustPropagate::kPasses; ++pass)
        {
            TrustPropagate::Constants constants = {
                .DstTexSize = desc.DstTexSize,
                .StepSize = 1 << pass,
                .Flags = 0,
                .DemodDivisorFloor = desc.DemodDivisorFloor
            };
            TrustPropagate::Input in = { .Resources = {
                .InAlbedoTrust = m_albedoTrust[read].Get(),
                .InLinearDepth = m_LinearDepth.Get(),
                .InNormals = outResources.Normals.Get()
            }};
            TrustPropagate::Output out = { .Resources = { .OutAlbedoTrust = m_albedoTrust[1 - read].Get() } };
            m_trustPropagateShader.Dispatch(cmdList, GetAsByteSpan(constants), in.AsArray, out.AsArray, dim);
            read = 1 - read;
        }
        m_albedoTrustResult = read;
    }

    void TransitionDenoiserOutputsToRead(ID3D12GraphicsCommandList* cmdList) noexcept
    {
        if (!cmdList)
            return;

        if (m_radianceOutputsInUavState)
        {
            // The direct-specular output shares the radiance outputs' state lifetime.
            std::array<ID3D12Resource*, 4> buffers = { m_outputBuffer1.Get(), m_outputBuffer2.Get(),
                m_directSpecularOutput.Get(), m_indirectDiffuseOutput.Get() };
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
            std::array<ID3D12Resource*, 4> buffers = { m_outputBuffer1.Get(), m_outputBuffer2.Get(),
                m_directSpecularOutput.Get(), m_indirectDiffuseOutput.Get() };
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
                                    D3D12_RESOURCE_STATES dstState,
                                    uint32_t logicalWidth, uint32_t logicalHeight)
    {
        if (!cmdList || !dstTex || !m_ambientOcclusionOutputInUavState ||
            logicalWidth == 0 || logicalHeight == 0)
            throw std::runtime_error("Ambient-occlusion output is unavailable for publication");

        const D3D12_RESOURCE_DESC srcDesc = m_ambientOcclusionOutput->GetDesc();
        const D3D12_RESOURCE_DESC dstDesc = dstTex->GetDesc();
        if (srcDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            dstDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            logicalWidth > srcDesc.Width || logicalHeight > srcDesc.Height ||
            logicalWidth > dstDesc.Width || logicalHeight > dstDesc.Height ||
            dstDesc.DepthOrArraySize != 1 || dstDesc.MipLevels != 1)
        {
            throw std::runtime_error(
                "Ambient-occlusion destination is incompatible with the allocation ceiling");
        }

        AddBarrier(cmdList, m_ambientOcclusionOutput.Get(), kUavState,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
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
        const D3D12_BOX sourceBox {
            0, 0, 0, logicalWidth, logicalHeight, 1
        };
        cmdList->CopyTextureRegion(
            &dstLocation, 0, 0, 0, &srcLocation, &sourceBox);

        AddBarrier(cmdList, m_ambientOcclusionOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   kSrvState);
        if (dstState != D3D12_RESOURCE_STATE_COPY_DEST)
            AddBarrier(cmdList, dstTex, D3D12_RESOURCE_STATE_COPY_DEST, dstState);

        m_ambientOcclusionOutputInUavState = false;
    }

    ID3D12Resource* PrepareDebugViewOutput(ID3D12GraphicsCommandList* cmdList, UINT width, UINT height)
    {
        if (!cmdList || !m_pDev || width == 0 || height == 0)
            throw std::runtime_error("Invalid AMD RR debug-view output request");

        auto& target = AcquireTarget(m_debugTargets, cmdList, width, height, FSRDFormats::DebugView,
                                     L"FSR_RR_DebugView_Output");
        m_debugViewOutput = target.resource;
        m_debugViewWidth = width;
        m_debugViewHeight = height;
        if (!target.inUavState)
            AddBarrier(cmdList, target.resource.Get(), kSrvState, kUavState);
        target.inUavState = m_debugViewOutputInUavState = true;

        return m_debugViewOutput.Get();
    }

    void TransitionDebugViewOutputToRead(ID3D12GraphicsCommandList* cmdList) noexcept
    {
        std::lock_guard recordingLock(DlssNr::Submission::RecordingMutex());
        if (!cmdList || !m_debugViewOutput || !m_debugViewOutputInUavState)
            return;

        AddBarrier(cmdList, m_debugViewOutput.Get(), kUavState, kSrvState);
        m_debugViewOutputInUavState = false;
        for (auto& target : m_debugTargets)
            if (target.resource.Get() == m_debugViewOutput.Get()) target.inUavState = false;
    }

    void Blit(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* srcTex,
              ID3D12Resource* dstTex, XMFLOAT2 dstDim, XMFLOAT2 logicalSrcDim,
              XMFLOAT2 logicalSrcBase)
    {
        if (!cmdList || !srcTex || !dstTex)
            throw std::runtime_error("Blit requires a command list, source and destination");

        const D3D12_RESOURCE_DESC srcDesc = srcTex->GetDesc();
        const XMFLOAT2 physicalSrcDim {
            static_cast<float>(srcDesc.Width), static_cast<float>(srcDesc.Height)
        };
        if (logicalSrcDim.x == 0.0f || logicalSrcDim.y == 0.0f)
            logicalSrcDim = physicalSrcDim;

        if (dstDim.x == 0 || dstDim.y == 0)
        {
            D3D12_RESOURCE_DESC dstDesc = dstTex->GetDesc();
            dstDim.x = (float)dstDesc.Width;
            dstDim.y = (float)dstDesc.Height;
        }

        if (physicalSrcDim.x == 0.0f || physicalSrcDim.y == 0.0f ||
            logicalSrcDim.x <= 0.0f || logicalSrcDim.y <= 0.0f ||
            logicalSrcBase.x < 0.0f || logicalSrcBase.y < 0.0f ||
            logicalSrcBase.x + logicalSrcDim.x > physicalSrcDim.x ||
            logicalSrcBase.y + logicalSrcDim.y > physicalSrcDim.y ||
            dstDim.x <= 0.0f || dstDim.y <= 0.0f)
            throw std::runtime_error("Blit source subrect or destination extent is invalid");

        Composition::Input inputs = {};
        inputs.Resources.InIndirectSpecular = srcTex;

        const Composition::Constants constants = 
        {
            .DstTexSize = 
            {
                dstDim.x,           dstDim.y,
                (1.0f / dstDim.x),  (1.0f / dstDim.y)
            },
            .Flags = (UINT)CompFlags::RawSourceBlit | (UINT)CompFlags::ScaleSrc,
            .SourceUvScale = {
                logicalSrcDim.x / physicalSrcDim.x,
                logicalSrcDim.y / physicalSrcDim.y
            },
            .SourceUvOffset = {
                logicalSrcBase.x / physicalSrcDim.x,
                logicalSrcBase.y / physicalSrcDim.y
            }
        };

        std::array<ID3D12Resource*, Composition::kOutputCount> uavs { dstTex, nullptr, nullptr };
        const std::span<const byte> cbData((const byte*) &constants, sizeof(constants));

        m_compShader.Dispatch(cmdList, cbData, inputs.AsArray, uavs, dstDim, false);
    }

    void SetDescResources(ffxDispatchDescHeader& signalHeader, ffxDispatchDescDenoiser& dispatchDesc)
    {
        auto& outResources = m_out.Resources;

        dispatchDesc.header = 
        { 
            .type = FFX_API_DISPATCH_DESC_TYPE_DENOISER,
            .pNext = &signalHeader // Link signal desc to main header
        };

        // Always the converter's own field: it is the one whose sign convention is known,
        // and the one every other consumer reads - the floor seed writes it from the
        // title's published linear depth when one is provided, so the denoiser sees the
        // same geometry the packing and floor passes describe.
        dispatchDesc.linearDepth = ffxApiGetResourceDX12(
            m_LinearDepth.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatchDesc.motionVectors = ffxApiGetResourceDX12(outResources.Motion.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatchDesc.normals = ffxApiGetResourceDX12(
            outResources.Normals.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatchDesc.specularAlbedo = ffxApiGetResourceDX12(
            outResources.SpecAlbedo.Get(),
            FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        dispatchDesc.diffuseAlbedo = ffxApiGetResourceDX12(
            outResources.DiffAlbedo.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    }
};

// Public interface

FSRDPreprocessor_Dx12::FSRDPreprocessor_Dx12(std::string_view name, ID3D12Device* pDev) :
    m_impl(std::make_unique<Impl>()), 
    m_InstanceName(name),
    m_IsInitialized(false)
{
    try
    {
        m_impl->m_pDev = pDev;
        m_impl->Initialize(GetAsByteSpan(FSRDFloorSeed_cso), GetAsByteSpan(FSRDFloor_cso),
                           GetAsByteSpan(FSRDInputConv_cso),
                           GetAsByteSpan(FSRDOutputComp_cso));
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
    if (!m_IsInitialized)
        return false;
    try
    {
        return m_impl->DispatchConversion(cmdList, desc);
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("FSRD input conversion failed. Details: {}", err.what());
    }

    return false;
}

void FSRDPreprocessor_Dx12::SetStageTimings(FSRDStageTimings* timings)
{
    m_impl->m_stageTimings = timings;
}

void FSRDPreprocessor_Dx12::SetRuntimeSnapshot(FSRDRuntimeSnapshot* snapshot)
{
    m_impl->m_runtime = snapshot;
}

bool FSRDPreprocessor_Dx12::ConfigureSignalResources(bool extraDiffuse, bool extraSpecular, bool albedoRecovery,
                                                     bool diffuseCopy)
{
    if (m_impl->m_maxWidth != 0) return false;
    m_impl->m_extraDiffuse = extraDiffuse;
    m_impl->m_extraSpecular = extraSpecular;
    m_impl->m_albedoRecovery = albedoRecovery;
    // Conversion still writes u9 without the diffuse copy. Like every disabled slot it is a
    // 1x1 placeholder, and typed UAV stores outside it are discarded.
    m_impl->m_diffuseCopy = albedoRecovery && diffuseCopy;
    return true;
}

void FSRDPreprocessor_Dx12::GetSignals(ffxDispatchDescDenoiser& dispatchDesc,
                                       ffxDispatchDescDenoiserDirectDiffuse& directDiffuse,
                                       ffxDispatchDescDenoiserIndirectSpecular& indirectSpecular) const
{
    auto& outResources = m_impl->m_out.Resources;
    auto& signalData = outResources.Signals;
    ID3D12Resource* diffuseInput = signalData.DirectDiffuse.Get();
    ID3D12Resource* specularInput = signalData.IndirectSpecular.Get();

    directDiffuse =
    {
        .header =
        {
            .type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE,
            .pNext = &indirectSpecular.header
        },
        .signal =
        {
            .input = ffxApiGetResourceDX12(diffuseInput, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ),
            .output = ffxApiGetResourceDX12(m_impl->m_outputBuffer2.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS),
            .checkerboardOrigin = 0
        }
    };

    indirectSpecular =
    {
        .header = { .type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR },
        .signal =
        {
            .input = ffxApiGetResourceDX12(specularInput, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ),
            .output = ffxApiGetResourceDX12(m_impl->m_outputBuffer1.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS),
            .checkerboardOrigin = 0
        }
    };

    m_impl->SetDescResources(directDiffuse.header, dispatchDesc);
}

void FSRDPreprocessor_Dx12::GetDirectSpecularSignal(ffxDispatchDescDenoiserDirectSpecular& directSpecular, bool unmodulated) const
{
    directSpecular =
    {
        .header = { .type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR },
        .signal =
        {
            .input = ffxApiGetResourceDX12(unmodulated ? m_impl->m_out.Resources.DirectSpecular.Get()
                : m_impl->m_out.Resources.Signals.IndirectSpecular.Get(),
                                           FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ),
            .output = ffxApiGetResourceDX12(m_impl->m_directSpecularOutput.Get(),
                                            FFX_API_RESOURCE_STATE_UNORDERED_ACCESS),
            .checkerboardOrigin = 0
        }
    };
}

void FSRDPreprocessor_Dx12::GetIndirectDiffuseSignal(ffxDispatchDescDenoiserIndirectDiffuse& indirectDiffuse,
                                                     bool unmodulated) const
{
    indirectDiffuse = {
        .header = { .type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE },
        .signal = {
            .input = ffxApiGetResourceDX12(unmodulated ? m_impl->m_out.Resources.IndirectDiffuse.Get()
                                                       : m_impl->m_out.Resources.Signals.DirectDiffuse.Get(),
                                           FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ),
            .output = ffxApiGetResourceDX12(m_impl->m_indirectDiffuseOutput.Get(),
                                            FFX_API_RESOURCE_STATE_UNORDERED_ACCESS),
            .checkerboardOrigin = 0
        }
    };
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

void FSRDPreprocessor_Dx12::InvalidateCompositionHistory() noexcept
{
    std::lock_guard recordingLock(DlssNr::Submission::RecordingMutex());
    m_impl->m_historyValid = m_impl->m_historyPending = false;
    m_impl->m_canonicalDepthValid = m_impl->m_upscalerDepthValid = false;
    m_impl->m_conversionList = nullptr;
}

void FSRDPreprocessor_Dx12::FinishCompositionHistory(bool successfulNormalFrame) noexcept
{
    std::lock_guard recordingLock(DlssNr::Submission::RecordingMutex());
    if (successfulNormalFrame && m_impl->m_historyPending)
    {
        m_impl->m_historyPrevious = std::move(m_impl->m_historyNext);
        m_impl->m_historyValid = !m_impl->m_historyPrevious->discarded &&
                               !m_impl->m_historyPrevious->contentUncertain;
    }
    else m_impl->m_historyValid=false;
    m_impl->m_historyPending=false;
    m_impl->m_historyNext.reset();
    if (!successfulNormalFrame)
    {
        m_impl->m_canonicalDepthValid = m_impl->m_upscalerDepthValid = false;
        m_impl->m_conversionList = nullptr;
    }
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

bool FSRDPreprocessor_Dx12::CopyAmbientOcclusionOutput(
    ID3D12GraphicsCommandList* cmdList, ID3D12Resource* dstTex,
    D3D12_RESOURCE_STATES dstState, uint32_t logicalWidth, uint32_t logicalHeight)
{
    try
    {
        m_impl->CopyAmbientOcclusionOutput(
            cmdList, dstTex, dstState, logicalWidth, logicalHeight);
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

ID3D12Resource* FSRDPreprocessor_Dx12::PrepareDebugViewOutput(
    ID3D12GraphicsCommandList* cmdList, uint32_t width, uint32_t height)
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

void FSRDPreprocessor_Dx12::TransitionDebugViewOutputToRead(
    ID3D12GraphicsCommandList* cmdList) noexcept
{
    m_impl->TransitionDebugViewOutputToRead(cmdList);
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetDebugViewOutput() const
{
    return m_impl->m_debugViewOutput.Get();
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetCompositionOutput() const
{
    return m_impl->m_compositionOutput.Get();
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetDenoiserDiffuseOutput() const
{
    return m_impl->m_outputBuffer2.Get();
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetDenoiserDiffuseInputSignal() const
{
    return m_impl->m_out.Resources.Signals.DirectDiffuse.Get();
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetDenoiserSpecularOutput() const
{
    return m_impl->m_outputBuffer1.Get();
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetDenoiserNormalsInput() const
{
    return m_impl->m_out.Resources.Normals.Get();
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetDenoiserMotionInput() const
{
    return m_impl->m_out.Resources.Motion.Get();
}

void FSRDPreprocessor_Dx12::CompleteAdditiveCapture(ID3D12GraphicsCommandList* cmdList,
    const ffxDispatchDescDenoiser& dispatch, const CompositionDesc& composition,
    float preExposure, bool preExposureProvided,
    const SRDepthDiagnostic& srDepth) noexcept
{
    m_impl->CompleteAdditiveCapture(cmdList, dispatch, composition, preExposure, preExposureProvided, srDepth);
}

ID3D12Resource* FSRDPreprocessor_Dx12::PrepareUpscalerDepthInput(
    ID3D12GraphicsCommandList* cmdList, const UpscalerDepthEncode& encode)
{
    try
    {
        return m_impl->PrepareUpscalerDepthInput(cmdList, encode);
    }
    catch (const std::exception& err)
    {
        LOG_ERROR("FSRD SR depth encoding failed. Details: {}", err.what());
    }
    return nullptr;
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetUpscalerDepthInput() const
{
    return m_impl->m_upscalerDepthValid ? m_impl->m_upscalerDepthOutput.Get() : nullptr;
}

ID3D12Resource* FSRDPreprocessor_Dx12::GetDenoiserLinearDepthInput() const
{
    return m_impl->m_LinearDepth.Get();
}

bool FSRDPreprocessor_Dx12::Blit(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* srcTex,
                                 ID3D12Resource* dstTex, XMFLOAT2 dstDim,
                                 XMFLOAT2 logicalSrcDim, XMFLOAT2 logicalSrcBase) const

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
