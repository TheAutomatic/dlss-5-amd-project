#include <pch.h>

#include "Streamline_Hooks.h"

#include <shaders/reproject/mouseInputs/InputCollection.h>
#include <Util.h>
#include <Config.h>

#include <nvapi/fakenvapi.h>
#include <misc/IdentifyGpu.h>
#include <framegen/xefg/XeFG_Dx12.h>
#include <hooks/Reflex_Hooks.h>
#include <menu/menu_overlay_base.h>
#include <framegen/nvngx/Nvngx_FG.h>
#include <proxies/KernelBase_Proxy.h>
#include <imgui/ImGuiNotify.hpp>

#include <json.hpp>
#include <sl1_reflex.h>

// ---------------------------------------------------------------------------
// FSR-RR: signal-tag snapshot / diagnostics infrastructure (ported from fsr-rr reference)
// ---------------------------------------------------------------------------
#include <NVNGX_Parameter.h>
std::mutex StreamlineHooks::rrSignalTagMutex {};
RRSignalTagDiagnostics StreamlineHooks::rrSignalTagDiagnostics {};
std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, static_cast<size_t>(RRTaggedSignal::Count)>
    StreamlineHooks::rrTaggedD3D12Resources {};
SLTagInventoryDiagnostics StreamlineHooks::slTagInventoryDiagnostics {};
RRNGXPointerDiagnostics StreamlineHooks::rrNGXPointerDiagnostics {};

namespace
{
thread_local uint32_t g_rrActiveEvaluationFrame = UINT32_MAX;
thread_local uint32_t g_rrActiveEvaluationViewport = UINT32_MAX;

class ScopedRRActiveEvaluationFrame
{
  public:
    ScopedRRActiveEvaluationFrame(uint32_t frame, uint32_t viewport) noexcept
        : m_previousFrame(g_rrActiveEvaluationFrame), m_previousViewport(g_rrActiveEvaluationViewport)
    {
        g_rrActiveEvaluationFrame = frame;
        g_rrActiveEvaluationViewport = viewport;
    }

    ~ScopedRRActiveEvaluationFrame()
    {
        g_rrActiveEvaluationFrame = m_previousFrame;
        g_rrActiveEvaluationViewport = m_previousViewport;
    }

  private:
    uint32_t m_previousFrame;
    uint32_t m_previousViewport;
};
} // namespace

static bool TryGetRRTaggedSignal(sl::BufferType type, RRTaggedSignal& signal)
{
    switch (type)
    {
    case sl::kBufferTypeNormalRoughness:
        signal = RRTaggedSignal::NormalRoughness;
        return true;
    case sl::kBufferTypeEmissive:
        signal = RRTaggedSignal::Emissive;
        return true;
    case sl::kBufferTypeSpecularMotionVectors:
        signal = RRTaggedSignal::SpecularMotionVectors;
        return true;
    case sl::kBufferTypeReflectionMotionVectors:
        signal = RRTaggedSignal::ReflectionMotionVectors;
        return true;
    case sl::kBufferTypeSpecularHitDistance:
        signal = RRTaggedSignal::SpecularHitDistance;
        return true;
    case sl::kBufferTypeSpecularRayDirectionHitDistance:
        signal = RRTaggedSignal::SpecularRayDirectionHitDistance;
        return true;
    case sl::kBufferTypeLinearDepth:
        signal = RRTaggedSignal::LinearDepth;
        return true;
    case sl::kBufferTypeDiffuseHitNoisy:
        signal = RRTaggedSignal::DiffuseNoisy;
        return true;
    case sl::kBufferTypeDiffuseHitDenoised:
        signal = RRTaggedSignal::DiffuseDenoised;
        return true;
    case sl::kBufferTypeSpecularHitNoisy:
        signal = RRTaggedSignal::SpecularNoisy;
        return true;
    case sl::kBufferTypeSpecularHitDenoised:
        signal = RRTaggedSignal::SpecularDenoised;
        return true;
    case sl::kBufferTypeShadowNoisy:
        signal = RRTaggedSignal::ShadowNoisy;
        return true;
    case sl::kBufferTypeShadowDenoised:
        signal = RRTaggedSignal::ShadowDenoised;
        return true;
    case sl::kBufferTypeAmbientOcclusionNoisy:
        signal = RRTaggedSignal::AmbientOcclusionNoisy;
        return true;
    case sl::kBufferTypeAmbientOcclusionDenoised:
        signal = RRTaggedSignal::AmbientOcclusionDenoised;
        return true;
    case sl::kBufferTypeShadowHint:
        signal = RRTaggedSignal::ShadowHint;
        return true;
    case sl::kBufferTypeReflectionHint:
        signal = RRTaggedSignal::ReflectionHint;
        return true;
    default:
        return false;
    }
}

static bool IsRRCheckerboardInput(RRTaggedSignal signal)
{
    return signal == RRTaggedSignal::DiffuseNoisy || signal == RRTaggedSignal::SpecularNoisy ||
           signal == RRTaggedSignal::ShadowNoisy || signal == RRTaggedSignal::AmbientOcclusionNoisy;
}

static const char* GetRRTagSourceName(RRTagSource source)
{
    switch (source)
    {
    case RRTagSource::SetTag:
        return "slSetTag";
    case RRTagSource::SetTagForFrame:
        return "slSetTagForFrame";
    case RRTagSource::EvaluateFeature:
        return "slEvaluateFeature";
    default:
        return "unknown";
    }
}

const char* StreamlineHooks::getSLBufferTypeName(sl::BufferType type)
{
    static constexpr std::array<const char*, 68> names { "Depth",
                                                         "MotionVectors",
                                                         "HUDLessColor",
                                                         "ScalingInputColor",
                                                         "ScalingOutputColor",
                                                         "Normals",
                                                         "Roughness",
                                                         "Albedo",
                                                         "SpecularAlbedo",
                                                         "IndirectAlbedo",
                                                         "SpecularMotionVectors",
                                                         "DisocclusionMask",
                                                         "Emissive",
                                                         "Exposure",
                                                         "NormalRoughness",
                                                         "DiffuseHitNoisy",
                                                         "DiffuseHitDenoised",
                                                         "SpecularHitNoisy",
                                                         "SpecularHitDenoised",
                                                         "ShadowNoisy",
                                                         "ShadowDenoised",
                                                         "AmbientOcclusionNoisy",
                                                         "AmbientOcclusionDenoised",
                                                         "UIColorAndAlpha",
                                                         "ShadowHint",
                                                         "ReflectionHint",
                                                         "ParticleHint",
                                                         "TransparencyHint",
                                                         "AnimatedTextureHint",
                                                         "BiasCurrentColorHint",
                                                         "RaytracingDistance",
                                                         "ReflectionMotionVectors",
                                                         "Position",
                                                         "InvalidDepthMotionHint",
                                                         "Alpha",
                                                         "OpaqueColor",
                                                         "ReactiveMaskHint",
                                                         "TransparencyAndCompositionMaskHint",
                                                         "ReflectedAlbedo",
                                                         "ColorBeforeParticles",
                                                         "ColorBeforeTransparency",
                                                         "ColorBeforeFog",
                                                         "SpecularHitDistance",
                                                         "SpecularRayDirectionHitDistance",
                                                         "SpecularRayDirection",
                                                         "DiffuseHitDistance",
                                                         "DiffuseRayDirectionHitDistance",
                                                         "DiffuseRayDirection",
                                                         "HiResDepth",
                                                         "LinearDepth",
                                                         "BidirectionalDistortionField",
                                                         "TransparencyLayer",
                                                         "TransparencyLayerOpacity",
                                                         "Backbuffer",
                                                         "NoWarpMask",
                                                         "ColorAfterParticles",
                                                         "ColorAfterTransparency",
                                                         "ColorAfterFog",
                                                         "ScreenSpaceSubsurfaceScatteringGuide",
                                                         "ColorBeforeScreenSpaceSubsurfaceScattering",
                                                         "ColorAfterScreenSpaceSubsurfaceScattering",
                                                         "ScreenSpaceRefractionGuide",
                                                         "ColorBeforeScreenSpaceRefraction",
                                                         "ColorAfterScreenSpaceRefraction",
                                                         "DepthOfFieldGuide",
                                                         "ColorBeforeDepthOfField",
                                                         "ColorAfterDepthOfField",
                                                         "ScalingOutputAlpha" };

    return type < names.size() ? names[type] : "Unknown/custom";
}

const char* StreamlineHooks::getRRTaggedSignalName(RRTaggedSignal signal)
{
    switch (signal)
    {
    case RRTaggedSignal::NormalRoughness:
        return "NormalRoughness";
    case RRTaggedSignal::Emissive:
        return "Emissive";
    case RRTaggedSignal::SpecularMotionVectors:
        return "SpecularMotionVectors";
    case RRTaggedSignal::ReflectionMotionVectors:
        return "ReflectionMotionVectors";
    case RRTaggedSignal::SpecularHitDistance:
        return "SpecularHitDistance";
    case RRTaggedSignal::SpecularRayDirectionHitDistance:
        return "SpecularRayDirectionHitDistance";
    case RRTaggedSignal::LinearDepth:
        return "LinearDepth";
    case RRTaggedSignal::DiffuseNoisy:
        return "Diffuse.Noisy";
    case RRTaggedSignal::DiffuseDenoised:
        return "Diffuse.Denoised";
    case RRTaggedSignal::SpecularNoisy:
        return "Specular.Noisy";
    case RRTaggedSignal::SpecularDenoised:
        return "Specular.Denoised";
    case RRTaggedSignal::ShadowNoisy:
        return "Shadow.Noisy";
    case RRTaggedSignal::ShadowDenoised:
        return "Shadow.Denoised";
    case RRTaggedSignal::AmbientOcclusionNoisy:
        return "AmbientOcclusion.Noisy";
    case RRTaggedSignal::AmbientOcclusionDenoised:
        return "AmbientOcclusion.Denoised";
    case RRTaggedSignal::ShadowHint:
        return "ShadowHint";
    case RRTaggedSignal::ReflectionHint:
        return "ReflectionHint";
    default:
        return "Unknown";
    }
}

const char* StreamlineHooks::getRRPreferredTagFormat(RRTaggedSignal signal)
{
    switch (signal)
    {
    case RRTaggedSignal::NormalRoughness:
        return "game-defined";
    case RRTaggedSignal::Emissive:
        return "shader-readable color texture";
    case RRTaggedSignal::SpecularMotionVectors:
    case RRTaggedSignal::ReflectionMotionVectors:
        return "DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_FLOAT, "
               "DXGI_FORMAT_R16G16B16A16_FLOAT, or DXGI_FORMAT_R32G32B32A32_FLOAT";
    case RRTaggedSignal::SpecularHitDistance:
        return "DXGI_FORMAT_R16_FLOAT or DXGI_FORMAT_R32_FLOAT";
    case RRTaggedSignal::SpecularRayDirectionHitDistance:
        return "DXGI_FORMAT_R16G16B16A16_FLOAT or DXGI_FORMAT_R32G32B32A32_FLOAT";
    case RRTaggedSignal::LinearDepth:
        return "DXGI_FORMAT_R32_FLOAT or DXGI_FORMAT_R16_FLOAT";
    case RRTaggedSignal::DiffuseNoisy:
    case RRTaggedSignal::DiffuseDenoised:
    case RRTaggedSignal::SpecularNoisy:
    case RRTaggedSignal::SpecularDenoised:
        return "DXGI_FORMAT_R16G16B16A16_FLOAT";
    case RRTaggedSignal::ShadowNoisy:
        return "DXGI_FORMAT_R16_FLOAT";
    case RRTaggedSignal::ShadowDenoised:
    case RRTaggedSignal::AmbientOcclusionNoisy:
    case RRTaggedSignal::AmbientOcclusionDenoised:
        return "DXGI_FORMAT_R8_UNORM";
    case RRTaggedSignal::ShadowHint:
    case RRTaggedSignal::ReflectionHint:
        return "hint only; not an RR signal";
    default:
        return "unknown";
    }
}

bool StreamlineHooks::isRRPreferredTagFormat(RRTaggedSignal signal, DXGI_FORMAT format)
{
    switch (signal)
    {
    case RRTaggedSignal::NormalRoughness:
        return format != DXGI_FORMAT_UNKNOWN;
    case RRTaggedSignal::Emissive:
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R11G11B10_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return true;
        default:
            return false;
        }
    case RRTaggedSignal::SpecularMotionVectors:
    case RRTaggedSignal::ReflectionMotionVectors:
        switch (format)
        {
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return true;
        default:
            return false;
        }
    case RRTaggedSignal::SpecularHitDistance:
        return format == DXGI_FORMAT_R16_FLOAT || format == DXGI_FORMAT_R32_FLOAT;
    case RRTaggedSignal::SpecularRayDirectionHitDistance:
        return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    case RRTaggedSignal::LinearDepth:
        return format == DXGI_FORMAT_R32_FLOAT || format == DXGI_FORMAT_R16_FLOAT;
    case RRTaggedSignal::DiffuseNoisy:
    case RRTaggedSignal::DiffuseDenoised:
    case RRTaggedSignal::SpecularNoisy:
    case RRTaggedSignal::SpecularDenoised:
        return format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    case RRTaggedSignal::ShadowNoisy:
        return format == DXGI_FORMAT_R16_FLOAT;
    case RRTaggedSignal::ShadowDenoised:
    case RRTaggedSignal::AmbientOcclusionNoisy:
    case RRTaggedSignal::AmbientOcclusionDenoised:
        return format == DXGI_FORMAT_R8_UNORM;
    default:
        return false;
    }
}

const char* StreamlineHooks::getRRCheckerboardAssessment(RRTaggedSignal signal,
                                                         const RRTaggedResourceDiagnostic& diagnostic,
                                                         uint32_t renderWidth, uint32_t renderHeight)
{
    if (!diagnostic.observed)
        return "not-observed";
    if (!diagnostic.present)
        return "cleared";
    if (!IsRRCheckerboardInput(signal))
        return "not-applicable";
    if (renderWidth == 0 || renderHeight == 0)
        return "render-size-unknown";
    if (diagnostic.effectiveHeight != renderHeight)
        return "resolution-mismatch";
    if (diagnostic.effectiveWidth == renderWidth)
        return "full-resolution";

    const uint64_t doubledWidth = static_cast<uint64_t>(diagnostic.effectiveWidth) * 2u;
    const uint64_t renderWidth64 = renderWidth;
    if (doubledWidth + 1u >= renderWidth64 && doubledWidth <= renderWidth64 + 1u)
        return "half-width-candidate";

    return "resolution-mismatch";
}

RRSignalTagDiagnostics StreamlineHooks::getRRSignalTagDiagnostics()
{
    std::scoped_lock lock(rrSignalTagMutex);
    return rrSignalTagDiagnostics;
}

Microsoft::WRL::ComPtr<ID3D12Resource> StreamlineHooks::getRRTaggedD3D12Resource(RRTaggedSignal signal)
{
    std::scoped_lock lock(rrSignalTagMutex);
    return rrTaggedD3D12Resources[static_cast<size_t>(signal)];
}

RRD3D12SignalTagSnapshot StreamlineHooks::getRRD3D12SignalTagSnapshot()
{
    std::scoped_lock lock(rrSignalTagMutex);
    RRD3D12SignalTagSnapshot snapshot {};
    snapshot.generation = rrSignalTagDiagnostics.generation;
    snapshot.activeEvaluationFrame = g_rrActiveEvaluationFrame;
    snapshot.activeEvaluationViewport = g_rrActiveEvaluationViewport;
    for (size_t i = 0; i < snapshot.resources.size(); ++i)
    {
        snapshot.resources[i].diagnostic = rrSignalTagDiagnostics.resources[i];
        snapshot.resources[i].resource = rrTaggedD3D12Resources[i];
    }
    return snapshot;
}

SLConstantsSnapshot StreamlineHooks::getSLConstantsSnapshot()
{
    std::scoped_lock lock(setConstantsMutex);
    const auto& state = State::Instance();
    return { .constants = state.slLastConstants,
             .frameIndex = state.slLastConstantsFrame,
             .viewport = state.slLastConstantsViewport };
}

void StreamlineHooks::resetRRSignalTagDiagnostics()
{
    std::scoped_lock lock(rrSignalTagMutex);
    rrSignalTagDiagnostics = {};
    rrTaggedD3D12Resources = {};
}

static bool HasResourceMetadataChanged(const RRTaggedResourceDiagnostic& previous,
                                       const RRTaggedResourceDiagnostic& next)
{
    return !previous.observed || previous.present != next.present || previous.debugName != next.debugName ||
           previous.nativeWidth != next.nativeWidth || previous.nativeHeight != next.nativeHeight ||
           previous.effectiveWidth != next.effectiveWidth || previous.effectiveHeight != next.effectiveHeight ||
           previous.extentLeft != next.extentLeft || previous.extentTop != next.extentTop ||
           previous.format != next.format || previous.dimension != next.dimension ||
           previous.resourceFlags != next.resourceFlags || previous.mipLevels != next.mipLevels ||
           previous.arraySize != next.arraySize || previous.sampleCount != next.sampleCount ||
           previous.state != next.state || previous.lifecycle != next.lifecycle || previous.viewport != next.viewport;
}

static std::string GetD3D12DebugObjectName(ID3D12Object* object)
{
    if (object == nullptr)
        return {};

    UINT nameSize = 0;
    object->GetPrivateData(WKPDID_D3DDebugObjectName, &nameSize, nullptr);
    if (nameSize > 1)
    {
        std::string name(nameSize, '\0');
        if (SUCCEEDED(object->GetPrivateData(WKPDID_D3DDebugObjectName, &nameSize, name.data())))
        {
            name.resize(strnlen(name.c_str(), name.size()));
            return name;
        }
    }

    nameSize = 0;
    object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &nameSize, nullptr);
    if (nameSize > sizeof(wchar_t))
    {
        std::vector<wchar_t> name(nameSize / sizeof(wchar_t), L'\0');
        if (SUCCEEDED(object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &nameSize, name.data())))
            return wstring_to_string(name.data());
    }

    return {};
}

static RRTaggedResourceDiagnostic CaptureSLResourceTag(const sl::ResourceTag& tag, uint32_t frameIndex,
                                                       uint32_t viewport, RRTagSource source)
{
    RRTaggedResourceDiagnostic result {};
    result.observed = true;
    result.present = tag.resource != nullptr && tag.resource->native != nullptr;
    result.frameIndex = frameIndex;
    result.viewport = viewport;
    result.source = source;
    result.lifecycle = tag.lifecycle;

    if (!result.present)
        return result;

    result.resourceAddress = tag.resource->native;
    result.state = tag.resource->state;

    auto* resource = reinterpret_cast<ID3D12Resource*>(tag.resource->native);
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    result.debugName = GetD3D12DebugObjectName(resource);
    result.nativeWidth = desc.Width;
    result.nativeHeight = desc.Height;
    result.format = desc.Format;
    result.dimension = desc.Dimension;
    result.resourceFlags = desc.Flags;
    result.mipLevels = desc.MipLevels;
    result.arraySize = desc.DepthOrArraySize;
    result.sampleCount = desc.SampleDesc.Count;

    result.usesExtent = static_cast<bool>(tag.extent);
    result.extentLeft = tag.extent.left;
    result.extentTop = tag.extent.top;
    result.effectiveWidth =
        result.usesExtent ? tag.extent.width : static_cast<uint32_t>(std::min<uint64_t>(desc.Width, UINT32_MAX));
    result.effectiveHeight = result.usesExtent ? tag.extent.height : desc.Height;
    return result;
}

SLTagInventoryDiagnostics StreamlineHooks::getSLTagInventoryDiagnostics()
{
    std::scoped_lock lock(rrSignalTagMutex);
    return slTagInventoryDiagnostics;
}

void StreamlineHooks::probeSLResourceTag(const sl::ResourceTag& tag, uint32_t frameIndex, uint32_t viewport,
                                         RRTagSource source)
{
    RRTaggedResourceDiagnostic next = CaptureSLResourceTag(tag, frameIndex, viewport, source);
    bool shouldLog = false;

    {
        std::scoped_lock lock(rrSignalTagMutex);
        auto entry = std::find_if(
            slTagInventoryDiagnostics.resources.begin(), slTagInventoryDiagnostics.resources.end(),
            [&tag](const SLTaggedResourceInventoryEntry& candidate) { return candidate.type == tag.type; });

        if (entry == slTagInventoryDiagnostics.resources.end())
        {
            slTagInventoryDiagnostics.resources.push_back({ tag.type, next });
            entry = std::prev(slTagInventoryDiagnostics.resources.end());
            shouldLog = true;
        }
        else
        {
            next.updateCount = entry->resource.updateCount + 1;
            shouldLog = HasResourceMetadataChanged(entry->resource, next);
            entry->resource = next;
        }

        if (entry->resource.updateCount == 0)
            entry->resource.updateCount = 1;

        std::sort(slTagInventoryDiagnostics.resources.begin(), slTagInventoryDiagnostics.resources.end(),
                  [](const SLTaggedResourceInventoryEntry& left, const SLTaggedResourceInventoryEntry& right)
                  { return left.type < right.type; });
        ++slTagInventoryDiagnostics.generation;
    }

    if (!shouldLog)
        return;

    if (!next.present)
    {
        LOG_INFO("[SL_TAG_INVENTORY] {} type={} ({}) frame={} cleared", GetRRTagSourceName(source), tag.type,
                 getSLBufferTypeName(tag.type), frameIndex);
        return;
    }

    const auto formatName = magic_enum::enum_name(next.format);
    LOG_INFO("[SL_TAG_INVENTORY] {} type={} ({}) frame={} ptr={}, debugName='{}', native={}x{}, effective={}x{}, "
             "extent=[{},{}], format={}({}), dimension={}, mips={}, arrays={}, samples={}, "
             "resourceFlags={:#x}, state={:#x}",
             GetRRTagSourceName(source), tag.type, getSLBufferTypeName(tag.type), frameIndex, next.resourceAddress,
             next.debugName, next.nativeWidth, next.nativeHeight, next.effectiveWidth, next.effectiveHeight,
             next.extentLeft, next.extentTop, formatName.empty() ? "UNKNOWN" : formatName,
             static_cast<uint32_t>(next.format), static_cast<uint32_t>(next.dimension), next.mipLevels, next.arraySize,
             next.sampleCount, static_cast<uint32_t>(next.resourceFlags), next.state);
}

void StreamlineHooks::logSLTagInventoryDiagnostics(uint32_t renderWidth, uint32_t renderHeight)
{
    const auto diagnostics = getSLTagInventoryDiagnostics();
    LOG_INFO("[SL_TAG_INVENTORY] snapshot: generation={}, observedTypes={}, render={}x{}", diagnostics.generation,
             diagnostics.resources.size(), renderWidth, renderHeight);

    for (const auto& entry : diagnostics.resources)
    {
        const auto& diagnostic = entry.resource;
        if (!diagnostic.present)
        {
            LOG_INFO("[SL_TAG_INVENTORY] snapshot type={} ({}): cleared, updates={}", entry.type,
                     getSLBufferTypeName(entry.type), diagnostic.updateCount);
            continue;
        }

        const auto formatName = magic_enum::enum_name(diagnostic.format);
        const bool fullResolution = renderWidth != 0 && renderHeight != 0 && diagnostic.effectiveWidth == renderWidth &&
                                    diagnostic.effectiveHeight == renderHeight;
        const bool halfWidthCandidate =
            renderWidth != 0 && renderHeight != 0 && diagnostic.effectiveHeight == renderHeight &&
            static_cast<uint64_t>(diagnostic.effectiveWidth) * 2u + 1u >= renderWidth &&
            static_cast<uint64_t>(diagnostic.effectiveWidth) * 2u <= static_cast<uint64_t>(renderWidth) + 1u;

        LOG_INFO("[SL_TAG_INVENTORY] snapshot type={} ({}): ptr={}, debugName='{}', effective={}x{}, native={}x{}, "
                 "format={}({}), state={:#x}, updates={}, resolutionClass={}",
                 entry.type, getSLBufferTypeName(entry.type), diagnostic.resourceAddress, diagnostic.debugName,
                 diagnostic.effectiveWidth, diagnostic.effectiveHeight, diagnostic.nativeWidth, diagnostic.nativeHeight,
                 formatName.empty() ? "UNKNOWN" : formatName, static_cast<uint32_t>(diagnostic.format),
                 diagnostic.state, diagnostic.updateCount,
                 fullResolution       ? "full-resolution"
                 : halfWidthCandidate ? "half-width-candidate"
                                      : "other");
    }
}

RRNGXPointerDiagnostics StreamlineHooks::getRRNGXPointerDiagnostics()
{
    std::scoped_lock lock(rrSignalTagMutex);
    return rrNGXPointerDiagnostics;
}

static const char* GetNGXPointerKindName(RRNGXPointerKind kind)
{
    switch (kind)
    {
    case RRNGXPointerKind::OpaquePointer:
        return "opaque-pointer";
    case RRNGXPointerKind::D3D11Resource:
        return "D3D11-resource";
    case RRNGXPointerKind::D3D12Resource:
        return "D3D12-resource";
    default:
        return "unknown";
    }
}

void StreamlineHooks::probeRRNGXPointerParameters(const NVSDK_NGX_Parameter& parameters)
{
    static std::atomic_uint64_t probeCalls = 0;
    const uint64_t probeCall = probeCalls.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lock(rrSignalTagMutex);
        if (rrNGXPointerDiagnostics.generation != 0 && probeCall % 120u != 0)
            return;
    }

    uint32_t allocationType = NGX_AllocTypes::Unknown;
    if (parameters.Get(NGX_AllocTypes::AllocKey.data(), &allocationType) != NVSDK_NGX_Result_Success ||
        (allocationType != NGX_AllocTypes::InternDynamic && allocationType != NGX_AllocTypes::InternPersistent))
    {
        std::scoped_lock lock(rrSignalTagMutex);
        rrNGXPointerDiagnostics.tableInspectable = false;
        return;
    }

    const auto& internalParameters = static_cast<const NVNGX_Parameters&>(parameters);
    const auto pointers = internalParameters.enumeratePointerParameters();

    std::vector<RRNGXPointerDiagnostic> nextEntries;
    nextEntries.reserve(pointers.size());
    for (const auto& pointer : pointers)
    {
        RRNGXPointerDiagnostic next {};
        next.name = pointer.name;
        next.present = pointer.address != nullptr;
        next.address = pointer.address;

        switch (pointer.kind)
        {
        case NGXPointerParameterKind::D3D11Resource:
            next.kind = RRNGXPointerKind::D3D11Resource;
            break;
        case NGXPointerParameterKind::D3D12Resource:
            next.kind = RRNGXPointerKind::D3D12Resource;
            break;
        default:
            next.kind = RRNGXPointerKind::OpaquePointer;
            break;
        }

        // Only exact D3D12 resource values are dereferenced. NGX void pointers
        // can also contain matrices, callbacks, or backend-specific objects.
        if (next.present && next.kind == RRNGXPointerKind::D3D12Resource)
        {
            const auto desc = static_cast<ID3D12Resource*>(pointer.address)->GetDesc();
            next.nativeWidth = desc.Width;
            next.nativeHeight = desc.Height;
            next.format = desc.Format;
            next.dimension = desc.Dimension;
            next.resourceFlags = desc.Flags;
            next.mipLevels = desc.MipLevels;
            next.arraySize = desc.DepthOrArraySize;
            next.sampleCount = desc.SampleDesc.Count;
        }

        nextEntries.push_back(std::move(next));
    }

    std::sort(nextEntries.begin(), nextEntries.end(),
              [](const RRNGXPointerDiagnostic& left, const RRNGXPointerDiagnostic& right)
              { return left.name < right.name; });

    std::vector<RRNGXPointerDiagnostic> changedEntries;
    {
        std::scoped_lock lock(rrSignalTagMutex);
        for (auto& next : nextEntries)
        {
            const auto previous =
                std::find_if(rrNGXPointerDiagnostics.parameters.begin(), rrNGXPointerDiagnostics.parameters.end(),
                             [&next](const RRNGXPointerDiagnostic& candidate) { return candidate.name == next.name; });

            if (previous != rrNGXPointerDiagnostics.parameters.end())
            {
                next.updateCount = previous->updateCount + 1;
                const bool changed =
                    previous->kind != next.kind || previous->present != next.present ||
                    previous->nativeWidth != next.nativeWidth || previous->nativeHeight != next.nativeHeight ||
                    previous->format != next.format || previous->dimension != next.dimension ||
                    previous->resourceFlags != next.resourceFlags || previous->mipLevels != next.mipLevels ||
                    previous->arraySize != next.arraySize || previous->sampleCount != next.sampleCount;
                if (changed)
                    changedEntries.push_back(next);
            }
            else
            {
                next.updateCount = 1;
                changedEntries.push_back(next);
            }
        }

        rrNGXPointerDiagnostics.tableInspectable = true;
        rrNGXPointerDiagnostics.parameters = nextEntries;
        ++rrNGXPointerDiagnostics.generation;
    }

    for (const auto& entry : changedEntries)
    {
        if (entry.kind != RRNGXPointerKind::D3D12Resource || !entry.present)
        {
            LOG_INFO("[RR_NGX_INVENTORY] key='{}', kind={}, ptr={}, present={}", entry.name,
                     GetNGXPointerKindName(entry.kind), entry.address, entry.present);
            continue;
        }

        const auto formatName = magic_enum::enum_name(entry.format);
        LOG_INFO("[RR_NGX_INVENTORY] key='{}', kind={}, ptr={}, size={}x{}, format={}({}), "
                 "dimension={}, mips={}, arrays={}, samples={}, resourceFlags={:#x}",
                 entry.name, GetNGXPointerKindName(entry.kind), entry.address, entry.nativeWidth, entry.nativeHeight,
                 formatName.empty() ? "UNKNOWN" : formatName, static_cast<uint32_t>(entry.format),
                 static_cast<uint32_t>(entry.dimension), entry.mipLevels, entry.arraySize, entry.sampleCount,
                 static_cast<uint32_t>(entry.resourceFlags));
    }
}

void StreamlineHooks::logRRNGXPointerDiagnostics()
{
    const auto diagnostics = getRRNGXPointerDiagnostics();
    LOG_INFO("[RR_NGX_INVENTORY] snapshot: inspectable={}, generation={}, pointerParameters={}",
             diagnostics.tableInspectable, diagnostics.generation, diagnostics.parameters.size());

    for (const auto& entry : diagnostics.parameters)
    {
        if (entry.kind != RRNGXPointerKind::D3D12Resource || !entry.present)
        {
            LOG_INFO("[RR_NGX_INVENTORY] snapshot key='{}': kind={}, ptr={}, present={}, updates={}", entry.name,
                     GetNGXPointerKindName(entry.kind), entry.address, entry.present, entry.updateCount);
            continue;
        }

        const auto formatName = magic_enum::enum_name(entry.format);
        LOG_INFO("[RR_NGX_INVENTORY] snapshot key='{}': kind={}, ptr={}, size={}x{}, "
                 "format={}({}), dimension={}, updates={}",
                 entry.name, GetNGXPointerKindName(entry.kind), entry.address, entry.nativeWidth, entry.nativeHeight,
                 formatName.empty() ? "UNKNOWN" : formatName, static_cast<uint32_t>(entry.format),
                 static_cast<uint32_t>(entry.dimension), entry.updateCount);
    }
}

void StreamlineHooks::resetRRInputInventoryDiagnostics()
{
    std::scoped_lock lock(rrSignalTagMutex);
    slTagInventoryDiagnostics = {};
    rrNGXPointerDiagnostics = {};
}

void StreamlineHooks::probeRRResourceTag(const sl::ResourceTag& tag, uint32_t frameIndex, uint32_t viewport,
                                         RRTagSource source)
{
    RRTaggedSignal signal;
    if (!TryGetRRTaggedSignal(tag.type, signal))
        return;

    RRTaggedResourceDiagnostic next = CaptureSLResourceTag(tag, frameIndex, viewport, source);

    RRTaggedResourceDiagnostic previous {};
    bool shouldLog = false;
    {
        std::scoped_lock lock(rrSignalTagMutex);
        auto& stored = rrSignalTagDiagnostics.resources[static_cast<size_t>(signal)];
        previous = stored;
        next.updateCount = previous.updateCount + 1;

        shouldLog = HasResourceMetadataChanged(previous, next);

        stored = next;
        auto& retainedResource = rrTaggedD3D12Resources[static_cast<size_t>(signal)];
        // eOnlyValidNow submitted through SetTag cannot be cached after that call
        // returns. A local ResourceTag passed directly to EvaluateFeature remains
        // inside its declaring call, however, and is retained only until the local
        // overlay is restored when that evaluation returns.
        retainedResource = next.present && (next.lifecycle != sl::ResourceLifecycle::eOnlyValidNow ||
                                            source == RRTagSource::EvaluateFeature)
                               ? reinterpret_cast<ID3D12Resource*>(next.resourceAddress)
                               : nullptr;
        ++rrSignalTagDiagnostics.generation;
    }

    if (!shouldLog)
        return;

    if (!next.present)
    {
        LOG_INFO("[RR_TAG_DIAG] {} {} frame={} cleared", GetRRTagSourceName(source), getRRTaggedSignalName(signal),
                 frameIndex);
        return;
    }

    const auto formatName = magic_enum::enum_name(next.format);
    const auto lifecycleName = magic_enum::enum_name(next.lifecycle);
    LOG_INFO("[RR_TAG_DIAG] {} {} frame={} ptr={}, debugName='{}', native={}x{}, effective={}x{}, "
             "extent=[{},{}], format={}({}), dimension={}, mips={}, arrays={}, samples={}, "
             "resourceFlags={:#x}, state={:#x}, lifecycle={}, preferredFormat={} ({})",
             GetRRTagSourceName(source), getRRTaggedSignalName(signal), frameIndex, next.resourceAddress,
             next.debugName, next.nativeWidth, next.nativeHeight, next.effectiveWidth, next.effectiveHeight,
             next.extentLeft, next.extentTop, formatName.empty() ? "UNKNOWN" : formatName,
             static_cast<uint32_t>(next.format), static_cast<uint32_t>(next.dimension), next.mipLevels, next.arraySize,
             next.sampleCount, static_cast<uint32_t>(next.resourceFlags), next.state,
             lifecycleName.empty() ? "UNKNOWN" : lifecycleName, isRRPreferredTagFormat(signal, next.format),
             getRRPreferredTagFormat(signal));
}

void StreamlineHooks::logRRSignalTagDiagnostics(uint32_t renderWidth, uint32_t renderHeight)
{
    const RRSignalTagDiagnostics diagnostics = getRRSignalTagDiagnostics();
    LOG_INFO("[RR_TAG_DIAG] snapshot: generation={}, render={}x{}", diagnostics.generation, renderWidth, renderHeight);

    for (size_t i = 0; i < diagnostics.resources.size(); ++i)
    {
        const auto signal = static_cast<RRTaggedSignal>(i);
        const auto& diagnostic = diagnostics.resources[i];
        if (!diagnostic.observed)
        {
            LOG_INFO("[RR_TAG_DIAG] snapshot {}: not observed", getRRTaggedSignalName(signal));
            continue;
        }

        if (!diagnostic.present)
        {
            LOG_INFO("[RR_TAG_DIAG] snapshot {}: cleared, updates={}", getRRTaggedSignalName(signal),
                     diagnostic.updateCount);
            continue;
        }

        const auto formatName = magic_enum::enum_name(diagnostic.format);
        LOG_INFO("[RR_TAG_DIAG] snapshot {}: ptr={}, debugName='{}', effective={}x{}, native={}x{}, "
                 "format={}({}), state={:#x}, updates={}, checkerboard={}, preferredFormat={} ({})",
                 getRRTaggedSignalName(signal), diagnostic.resourceAddress, diagnostic.debugName,
                 diagnostic.effectiveWidth, diagnostic.effectiveHeight, diagnostic.nativeWidth, diagnostic.nativeHeight,
                 formatName.empty() ? "UNKNOWN" : formatName, static_cast<uint32_t>(diagnostic.format),
                 diagnostic.state, diagnostic.updateCount,
                 getRRCheckerboardAssessment(signal, diagnostic, renderWidth, renderHeight),
                 isRRPreferredTagFormat(signal, diagnostic.format), getRRPreferredTagFormat(signal));
    }
}
#include <shaders/reproject/mouseInputs/InputCollection.h>

#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

#include <magic_enum.hpp>
#include "detours/detours.h"

static bool IsSL1AndFGActive()
{
    const auto& state = State::Instance();

    return state.streamlineVersion.major == 1 && state.activeFgInput == FGInput::DLSSG &&
           state.activeFgOutput != FGOutput::NoFG;
}

// Game's SL1 DLSSG has to stay inert when we use its inputs or provide DLSSG ourselves,
// otherwise its swapchain hooks end up stacked on top of our FG swapchain
static bool IsSL1AndDLSSGManaged()
{
    const auto& state = State::Instance();

    return state.streamlineVersion.major == 1 &&
           (state.activeFgInput == FGInput::DLSSG || state.activeFgOutput == FGOutput::DLSSG);
}

static void PatchSL1PluginJson(nlohmann::json& configJson)
{
    if (!IsSL1AndDLSSGManaged())
        return;

    LOG_DEBUG("Patching SL1 plugin JSON for external FG management");

    if (configJson.contains("/hooks"_json_pointer))
        configJson["hooks"].clear();

    if (configJson.contains("/exclusive_hooks"_json_pointer))
        configJson["exclusive_hooks"].clear();

    if (configJson.contains("/external/feature/tags"_json_pointer))
        configJson["external"]["feature"]["tags"].clear();

    if (configJson.contains("/vsync/supported"_json_pointer))
        configJson["vsync"]["supported"] = true;

    if (configJson.contains("/external/hws/required"_json_pointer))
        configJson["external"]["hws"]["required"] = false;
}

// Plugin may fail (denylist etc.) and leave pluginJSON empty
static bool HasPluginJson(bool result, const char** pluginJSON)
{
    return result && pluginJSON != nullptr && *pluginJSON != nullptr && **pluginJSON != '\0';
}

// SystemCaps lives inside sl.common's image. When Streamline drops one sl.common copy and keeps another,
// a pointer we got earlier can point into an unmapped (or reused) range, so check before every access.
static bool IsWritableMemory(const void* address, size_t size)
{
    if (address == nullptr)
        return false;

    MEMORY_BASIC_INFORMATION mbi {};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi))
        return false;

    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;

    constexpr DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & writable) == 0)
        return false;

    const auto end = reinterpret_cast<uintptr_t>(address) + size;
    const auto regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end <= regionEnd;
}

char* StreamlineHooks::trimStreamlineLog(const char* msg)
{
    char* result = (char*) malloc(strlen(msg) + 1);
    if (!result)
        return nullptr;

    strcpy(result, msg);

    size_t length = strlen(result);
    if (length > 0 && result[length - 1] == '\n')
    {
        result[length - 1] = '\0';
    }

    return result;
}

void StreamlineHooks::streamlineLogCallback(sl::LogType type, const char* msg)
{
    if (msg == nullptr)
        return;

    char* trimmed_msg = trimStreamlineLog(msg);
    if (trimmed_msg != nullptr)
    {
        switch (type)
        {
        case sl::LogType::eWarn:
            LOG_WARN("{}", trimmed_msg);
            break;
        case sl::LogType::eInfo:
            // SL info is noisy (ota.cpp startup, presentCommon VRAM every frame).
            // Keep it only at LogLevel=3; info stays readable.
            LOG_DEBUG("{}", trimmed_msg);
            break;
        case sl::LogType::eError:
            LOG_ERROR("{}", trimmed_msg);
            break;
        case sl::LogType::eCount:
            LOG_ERROR("{}", trimmed_msg);
            break;
        }

        free(trimmed_msg);
    }

    if (o_logCallback != nullptr)
        o_logCallback(type, msg);
}

sl::Result StreamlineHooks::hkslInit(const sl::Preferences& pref, uint64_t sdkVersion)
{
    LOG_FUNC();

    sl::Preferences localPref = pref;

    if (localPref.logMessageCallback != &streamlineLogCallback)
        o_logCallback = localPref.logMessageCallback;
    localPref.logLevel = sl::LogLevel::eCount;
    localPref.logMessageCallback = &streamlineLogCallback;

    // renderAPI is optional so need to be careful, should only matter for Vulkan
    renderApi = localPref.renderAPI;

    State::Instance().slFGInputs.reportEngineType(localPref.engine);

    // Treat engine type set in Streamline as ground truth
    if (localPref.engine == sl::EngineType::eUnreal)
        State::Instance().gameQuirks |= GameQuirk::ForceUnrealEngine;

    std::filesystem::path localSlPath(Config::Instance()->MainDllPath.value());
    localSlPath = localSlPath / L"streamline"; // Hardcoded streamline folder

    auto localSlPathStr = localSlPath.wstring();

    std::vector<const wchar_t*> storage;

    // Replace the SL files to allow for MFG
    if (State::Instance().activeFgInput == FGInput::NvngxFG && std::filesystem::exists(localSlPath / L"sl.common.dll"))
    {
        storage.assign(localPref.pathsToPlugins, localPref.pathsToPlugins + localPref.numPathsToPlugins);

        std::filesystem::path pluginsDir;

        // Find the first path that contains sl.common.dll
        // If storage is empty, look in the exe folder. pathsToPlugins is an optional field
        if (storage.empty())
        {
            std::filesystem::path exeFolder = Util::ExePath().parent_path();
            if (std::filesystem::exists(exeFolder / L"sl.common.dll"))
            {
                pluginsDir = exeFolder;
            }
        }
        else
        {
            for (const wchar_t* pathStr : storage)
            {
                if (!pathStr)
                    continue;

                std::filesystem::path p = pathStr;
                if (std::filesystem::exists(p / L"sl.common.dll"))
                {
                    pluginsDir = p;
                    break;
                }
            }
        }

        std::vector<std::string> missingDlls;
        bool hasNewerPlugin = false;

        // If we found the plugins folder, scan its contents
        if (!pluginsDir.empty() && std::filesystem::exists(pluginsDir))
        {
            for (const auto& entry : std::filesystem::directory_iterator(pluginsDir))
            {
                if (!entry.is_regular_file())
                    continue;

                std::wstring filename = entry.path().filename().wstring();

                std::wstring lowerName = filename;
                to_lower_in_place(lowerName);

                // Skip interposer
                if (lowerName == L"sl.interposer.dll")
                    continue;

                const bool isSlDll = lowerName.starts_with(L"sl.") && lowerName.ends_with(L".dll");
                const bool isNvLowLatency = lowerName == L"nvlowlatencyvk.dll";

                if (isSlDll || isNvLowLatency)
                {
                    std::filesystem::path localDllPath = localSlPath / filename;

                    // Check if localSlPath also has this DLL
                    if (!std::filesystem::exists(localDllPath))
                    {
                        missingDlls.push_back(entry.path().filename().string());
                    }
                    else
                    {
                        // Compare versions
                        version_t pluginVer, pluginProdVer;
                        version_t localVer, localProdVer;

                        bool gotPluginVer = Util::GetFileVersion(entry.path().wstring(), &pluginVer, &pluginProdVer);
                        bool gotLocalVer = Util::GetFileVersion(localDllPath.wstring(), &localVer, &localProdVer);

                        if (gotPluginVer && gotLocalVer)
                        {
                            if (localVer > pluginVer)
                            {
                                hasNewerPlugin = true;
                            }
                        }
                    }
                }
            }
        }

        // Insert local path only if a newer plugin was found
        if (hasNewerPlugin)
        {
            LOG_DEBUG("Making the game use local streamline files");

            storage.insert(storage.begin(), localSlPathStr.c_str());
            localPref.pathsToPlugins = storage.data();
            localPref.numPathsToPlugins = (uint32_t) storage.size();

            if (!missingDlls.empty())
            {
                std::string toastMsg = "You are missing the following dlls from the streamline folder:\n";
                for (const auto& missingDll : missingDlls)
                {
                    toastMsg += "- " + missingDll + "\n";
                }

                ImGui::InsertNotification({ ImGuiToastType::Warning, 20000, toastMsg.c_str() });
            }
        }
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgOutput == FGOutput::DLSSG)
    {
        std::vector<sl::Feature> localFeaturesToLoad(pref.featuresToLoad, pref.featuresToLoad + pref.numFeaturesToLoad);
        std::erase(localFeaturesToLoad, sl::kFeatureDLSS_G);

        localPref.featuresToLoad = localFeaturesToLoad.data();
        localPref.numFeaturesToLoad = localFeaturesToLoad.size();

        // return so that localFeaturesToLoad is valid
        return o_slInit(localPref, sdkVersion);
    }

    // bool hookSetTag =
    //     (State::Instance().activeFgInput == FGInput::NvngxFG || State::Instance().activeFgInput == FGInput::DLSSG);

    // if (hookSetTag)
    //     localPref->flags &= ~(sl::PreferenceFlags::eAllowOTA | sl::PreferenceFlags::eLoadDownloadedPlugins);

    // To prevent mixed up OTA situations
    // if (State::Instance().activeFgOutput == FGOutput::DLSSG)
    //{
    //    localPref.flags &= ~sl::PreferenceFlags::eAllowOTA;
    //    localPref.flags &= ~sl::PreferenceFlags::eLoadDownloadedPlugins;
    //}

    return o_slInit(localPref, sdkVersion);
}

sl::Result StreamlineHooks::hkslIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapterInfo)
{
    if (feature == sl::kFeatureDLSS_G)
        return sl::Result::eOk;

    return o_slIsFeatureSupported(feature, adapterInfo);
}

sl::Result StreamlineHooks::hkslIsFeatureLoaded(sl::Feature feature, bool& loaded)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        loaded = true;
        return sl::Result::eOk;
    }

    return o_slIsFeatureLoaded(feature, loaded);
}

sl::Result StreamlineHooks::hkslGetFeatureRequirements(sl::Feature feature, sl::FeatureRequirements& requirements)
{
    if (feature == sl::kFeatureDLSS_G)
        return sl::Result::eOk;

    return o_slGetFeatureRequirements(feature, requirements);
}

sl::Result StreamlineHooks::hkslGetFeatureVersion(sl::Feature feature, sl::FeatureVersion& version)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        version.versionSL = { State::Instance().streamlineVersion.major, State::Instance().streamlineVersion.minor,
                              State::Instance().streamlineVersion.patch };
        version.versionNGX = { 4, 2, 0 };

        return sl::Result::eOk;
    }

    return o_slGetFeatureVersion(feature, version);
}

// Games usually ask for the max before our FG swapchain exists, so the output can't tell us yet
static int OutputMaxInterpolationCount()
{
    auto& state = State::Instance();

    if (auto fg = state.CurrentFG(); fg != nullptr && fg->GetMaxInterpolationCount() > 1)
        return fg->GetMaxInterpolationCount();

    if (state.activeFgOutput == FGOutput::XeFG)
    {
        static int xefgMax = 0;
        static bool xefgQueried = false;

        // GetState can be called every frame, only try once
        if (!xefgQueried && state.currentD3D12Device != nullptr)
        {
            xefgQueried = true;
            xefgMax = XeFG_Dx12::QueryMaxInterpolationCount(state.currentD3D12Device);
        }

        if (xefgMax > 0)
            return xefgMax;
    }

    return 1;
}

// Reports Opti's FG output capabilities to the game when DLSSG is used as FG Input
static void FillDlssgInputState(sl::DLSSGState& state)
{
    auto fg = State::Instance().CurrentFG();

    if (fg != nullptr && fg->IsActive() && !fg->IsPaused())
        state.numFramesActuallyPresented = fg->GetInterpolatedFrameCount() + 1;
    else
        state.numFramesActuallyPresented = 1;

    if (state.structVersion >= sl::kStructVersion2)
        state.numFramesToGenerateMax = OutputMaxInterpolationCount();
}

static sl::Result dummy_slDLSSGGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                        const sl::DLSSGOptions* options)
{
    FillDlssgInputState(state);
    state.estimatedVRAMUsageInBytes = 300 * 1024 * 1024;

    if (state.structVersion >= sl::kStructVersion2)
        state.bIsVsyncSupportAvailable = sl::Boolean::eTrue;

    return sl::Result::eOk;
}

static void ReportStreamlineFGRequest(const sl::DLSSGOptions& options)
{
    auto& state = State::Instance();
    if (state.activeFgInput == FGInput::DLSSG)
    {
        // Capture options even at RequiredReadyFrames=0, so enabling the wait at runtime retains the request.
        const bool requested = options.mode == sl::DLSSGMode::eOn || options.mode == sl::DLSSGMode::eAuto ||
                               options.mode == sl::DLSSGMode::eDynamic;
        state.slFGInputs.SetFGRequested(requested);
    }
}

static sl::Result dummy_slDLSSGSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    ReportStreamlineFGRequest(options);
    auto& state = State::Instance();

    // Only reading v1 fields, safe for any struct version
    const bool enabled = options.mode != sl::DLSSGMode::eOff;
    const int count = std::max(static_cast<int>(options.numFramesToGenerate), 1);

    if (enabled != state.dlssgInputGameEnabled || count != state.dlssgInputGameInterpolationCount)
    {
        LOG_INFO("Game requested DLSSG mode: {}, numFramesToGenerate: {}", magic_enum::enum_name(options.mode),
                 options.numFramesToGenerate);

        if (enabled && !state.dlssgInputGameEnabled && Config::Instance()->FGFollowsGame())
            state.fgChanged = true;

        state.dlssgInputGameEnabled = enabled;
        state.dlssgInputGameInterpolationCount = count;
    }

    return sl::Result::eOk;
}

sl::Result StreamlineHooks::hkslGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        if (strcmp(functionName, "slDLSSGSetOptions") == 0)
        {
            function = &dummy_slDLSSGSetOptions;

            return sl::Result::eOk;
        }

        if (strcmp(functionName, "slDLSSGGetState") == 0)
        {
            function = &dummy_slDLSSGGetState;

            return sl::Result::eOk;
        }
    }

    return o_slGetFeatureFunction(feature, functionName, function);
}

sl::Result StreamlineHooks::hkslSetFeatureLoaded(sl::Feature feature, bool loaded)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        return sl::Result::eOk;
    }

    return o_slSetFeatureLoaded(feature, loaded);
}

sl::Result StreamlineHooks::hkslSetTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
                                       uint32_t numTags, sl::CommandBuffer* cmdBuffer)
{
    if (renderApi == sl::RenderAPI::eD3D11)
    {
        LOG_ERROR("hkslSetTag doesn't support DX11");
        return o_slSetTag(viewport, tags, numTags, cmdBuffer);
    }

    if (renderApi == sl::RenderAPI::eCount)
        LOG_WARN("Incomplete Streamline hooks");

    if (tags == nullptr)
    {
        LOG_WARN("Game trying to remove a tag");
        return o_slSetTag(viewport, tags, numTags, cmdBuffer);
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG &&
        State::Instance().gameQuirks[GameQuirk::IgnoreTagsWithoutHudlessForFG])
    {
        bool hasDepth = false;
        bool hasMVs = false;
        bool hasHudless = false;

        for (uint32_t i = 0; i < numTags; i++)
        {
            if (tags[i].resource == nullptr || tags[i].resource->native == nullptr)
                continue;

            if (tags[i].type == sl::kBufferTypeDepth)
                hasDepth = true;

            if (tags[i].type == sl::kBufferTypeMotionVectors)
                hasMVs = true;

            if (tags[i].type == sl::kBufferTypeHUDLessColor)
                hasHudless = true;
        }

        // Try to skip a DLSS call
        if (hasDepth && hasMVs && !hasHudless)
        {
            LOG_DEBUG("Skipping the FG tagging of potential DLSS resources");
            return o_slSetTag(viewport, tags, numTags, cmdBuffer);
        }
    }

    for (uint32_t i = 0; i < numTags; i++)
    {
        if (renderApi == sl::RenderAPI::eD3D12)
        {
            probeSLResourceTag(tags[i], UINT32_MAX, static_cast<uint32_t>(viewport), RRTagSource::SetTag);
            probeRRResourceTag(tags[i], UINT32_MAX, static_cast<uint32_t>(viewport), RRTagSource::SetTag);
        }

        const auto typeEnum = (BufferType) tags[i].type;

        if (tags[i].resource == nullptr || tags[i].resource->native == nullptr)
        {
            LOG_TRACE("Resource of type: {} is null, continuing", magic_enum::enum_name(typeEnum));
            continue;
        }

        // Cyberpunk hudless state fix for RDNA 2
        if (State::Instance().gameQuirks & GameQuirk::CyberpunkHudlessState && renderApi != sl::RenderAPI::eVulkan &&
            tags[i].resource->state ==
                (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
            tags[i].type == sl::kBufferTypeHUDLessColor)
        {
            tags[i].resource->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            LOG_TRACE("Changing hudless resource state");
        }

        if (State::Instance().activeFgInput == FGInput::DLSSG &&
            (tags[i].type == sl::kBufferTypeHUDLessColor || tags[i].type == sl::kBufferTypeDepth ||
             tags[i].type == sl::kBufferTypeHiResDepth || tags[i].type == sl::kBufferTypeLinearDepth ||
             tags[i].type == sl::kBufferTypeMotionVectors || tags[i].type == sl::kBufferTypeUIColorAndAlpha ||
             tags[i].type == sl::kBufferTypeBidirectionalDistortionField))
        {
            State::Instance().slFGInputs.reportResource(tags[i], cmdBuffer, 0);
        }
        else if (State::Instance().activeFgInput == FGInput::NvngxFG)
        {
            LOG_TRACE("Tagging resource of type: {}", magic_enum::enum_name(typeEnum));
        }
    }

    auto result = o_slSetTag(viewport, tags, numTags, cmdBuffer);
    return result;
}

sl::Result StreamlineHooks::hkslSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport,
                                               const sl::ResourceTag* resources, uint32_t numResources,
                                               sl::CommandBuffer* cmdBuffer)
{
    if (renderApi == sl::RenderAPI::eD3D11)
    {
        LOG_ERROR("hkslSetTagForFrame doesn't support DX11");
        return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    }

    if (renderApi == sl::RenderAPI::eCount)
        LOG_WARN("Incomplete Streamline hooks");

    if (resources == nullptr)
    {
        LOG_WARN("Game trying to remove a tag");
        return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    }

    LOG_DEBUG("frameIndex: {}", static_cast<uint32_t>(frame));

    if (State::Instance().activeFgInput == FGInput::DLSSG &&
        State::Instance().gameQuirks[GameQuirk::IgnoreTagsWithoutHudlessForFG])
    {
        bool hasDepth = false;
        bool hasMVs = false;
        bool hasHudless = false;

        for (uint32_t i = 0; i < numResources; i++)
        {
            if (resources[i].resource == nullptr || resources[i].resource->native == nullptr)
                continue;

            if (resources[i].type == sl::kBufferTypeDepth)
                hasDepth = true;

            if (resources[i].type == sl::kBufferTypeMotionVectors)
                hasMVs = true;

            if (resources[i].type == sl::kBufferTypeHUDLessColor)
                hasHudless = true;
        }

        // Try to skip a DLSS call
        if (hasDepth && hasMVs && !hasHudless)
        {
            LOG_DEBUG("Skipping the FG tagging of potential DLSS resources");
            return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
        }
    }

    for (uint32_t i = 0; i < numResources; i++)
    {
        if (renderApi == sl::RenderAPI::eD3D12)
        {
            probeSLResourceTag(resources[i], static_cast<uint32_t>(frame), static_cast<uint32_t>(viewport),
                               RRTagSource::SetTagForFrame);
            probeRRResourceTag(resources[i], static_cast<uint32_t>(frame), static_cast<uint32_t>(viewport),
                               RRTagSource::SetTagForFrame);
        }

        const auto typeEnum = (BufferType) resources[i].type;

        if (resources[i].resource == nullptr || resources[i].resource->native == nullptr)
        {
            LOG_TRACE("Resource of type: {} is null, continuing", magic_enum::enum_name(typeEnum));
            continue;
        }

        if (State::Instance().activeFgInput == FGInput::DLSSG &&
            (resources[i].type == sl::kBufferTypeHUDLessColor || resources[i].type == sl::kBufferTypeDepth ||
             resources[i].type == sl::kBufferTypeHiResDepth || resources[i].type == sl::kBufferTypeLinearDepth ||
             resources[i].type == sl::kBufferTypeMotionVectors || resources[i].type == sl::kBufferTypeUIColorAndAlpha ||
             resources[i].type == sl::kBufferTypeBidirectionalDistortionField))
        {
            State::Instance().slFGInputs.reportResource(resources[i], cmdBuffer,
                                                        (uint32_t) frame);
        }
        else if (State::Instance().activeFgInput == FGInput::NvngxFG)
        {
            LOG_TRACE("Tagging resource of type: {}", magic_enum::enum_name(typeEnum));
        }
    }

    auto result = o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    return result;
}

sl::Result StreamlineHooks::hkslEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame,
                                                const sl::BaseStructure** inputs, uint32_t numInputs,
                                                sl::CommandBuffer* cmdBuffer)
{
    LOG_DEBUG("slEvaluateFeature: feature {}", (int) feature);
    uint32_t activeViewport = UINT32_MAX;
    if (numInputs > 0 && inputs != nullptr)
    {
        for (uint32_t i = 0; i < numInputs; ++i)
        {
            if (inputs[i] != nullptr && inputs[i]->structType == sl::ViewportHandle::s_structType)
            {
                activeViewport = static_cast<uint32_t>(*reinterpret_cast<const sl::ViewportHandle*>(inputs[i]));
                break;
            }
        }
    }

    const ScopedRRActiveEvaluationFrame activeEvaluationFrame(static_cast<uint32_t>(frame), activeViewport);
    LOG_DEBUG("frameIndex: {}", static_cast<uint32_t>(frame));

    // ResourceTag inputs to slEvaluateFeature are local to this evaluation and
    // must not replace tags submitted through slSetTag/slSetTagForFrame. Overlay
    // them while the intercepted feature runs, then restore the global entries.
    constexpr size_t rrSignalCount = static_cast<size_t>(RRTaggedSignal::Count);
    std::array<bool, rrSignalCount> localTagOverrides {};
    std::array<RRTaggedResourceDiagnostic, rrSignalCount> savedGlobalDiagnostics {};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, rrSignalCount> savedGlobalResources {};
    if (renderApi == sl::RenderAPI::eD3D12 && numInputs > 0 && inputs != nullptr)
    {
        std::scoped_lock lock(rrSignalTagMutex);
        for (uint32_t i = 0; i < numInputs; ++i)
        {
            if (inputs[i] == nullptr || inputs[i]->structType != sl::ResourceTag::s_structType)
                continue;

            const auto* tag = reinterpret_cast<const sl::ResourceTag*>(inputs[i]);
            RRTaggedSignal signal;
            if (!TryGetRRTaggedSignal(tag->type, signal))
                continue;

            const size_t signalIndex = static_cast<size_t>(signal);
            if (localTagOverrides[signalIndex])
                continue;

            localTagOverrides[signalIndex] = true;
            savedGlobalDiagnostics[signalIndex] = rrSignalTagDiagnostics.resources[signalIndex];
            savedGlobalResources[signalIndex] = rrTaggedD3D12Resources[signalIndex];
        }
    }

    if (numInputs > 0 && inputs != nullptr)
    {
        for (uint32_t i = 0; i < numInputs; i++)
        {
            if (inputs[i] == nullptr)
                continue;

            if (inputs[i]->structType == sl::ResourceTag::s_structType)
            {
                auto tag = (const sl::ResourceTag*) inputs[i];

                if (renderApi == sl::RenderAPI::eD3D12)
                {
                    probeSLResourceTag(*tag, static_cast<uint32_t>(frame), activeViewport,
                                       RRTagSource::EvaluateFeature);
                    probeRRResourceTag(*tag, static_cast<uint32_t>(frame), activeViewport,
                                       RRTagSource::EvaluateFeature);
                }

                if (State::Instance().activeFgInput == FGInput::DLSSG &&
                    (tag->type == sl::kBufferTypeHUDLessColor || tag->type == sl::kBufferTypeDepth ||
                     tag->type == sl::kBufferTypeHiResDepth || tag->type == sl::kBufferTypeLinearDepth ||
                     tag->type == sl::kBufferTypeMotionVectors || tag->type == sl::kBufferTypeUIColorAndAlpha ||
                     tag->type == sl::kBufferTypeBidirectionalDistortionField))
                {
                    State::Instance().slFGInputs.reportResource(*tag, cmdBuffer, (uint32_t) frame);
                }
            }
        }
    }

    // Some games attach FG resources to DLSS evaluation and never evaluate DLSS-G itself.
    if (State::Instance().activeFgInput == FGInput::DLSSG &&
        (feature == sl::kFeatureDLSS || feature == sl::kFeatureDLSS_G))
        State::Instance().slFGInputs.ReportEvaluate(static_cast<uint32_t>(frame));

    auto result = o_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);

    if (renderApi == sl::RenderAPI::eD3D12)
    {
        std::scoped_lock lock(rrSignalTagMutex);
        bool resourcesChanged = false;
        for (size_t i = 0; i < rrSignalCount; ++i)
        {
            if (localTagOverrides[i])
            {
                rrSignalTagDiagnostics.resources[i] = std::move(savedGlobalDiagnostics[i]);
                rrTaggedD3D12Resources[i] = std::move(savedGlobalResources[i]);
                resourcesChanged = true;
            }

            const auto& diagnostic = rrSignalTagDiagnostics.resources[i];
            const bool belongsToEvaluation =
                diagnostic.viewport == activeViewport &&
                (diagnostic.frameIndex == UINT32_MAX || diagnostic.frameIndex == static_cast<uint32_t>(frame));
            if (belongsToEvaluation && diagnostic.lifecycle == sl::ResourceLifecycle::eValidUntilEvaluate &&
                rrTaggedD3D12Resources[i])
            {
                // The declared lifetime ends as this evaluation returns. Keep
                // the last metadata for diagnostics, but release the retained
                // native resource so it cannot be rebound by a later frame.
                rrTaggedD3D12Resources[i].Reset();
                resourcesChanged = true;
            }
        }

        if (resourcesChanged)
            ++rrSignalTagDiagnostics.generation;
    }

    return result;
}

sl::Result StreamlineHooks::hkslAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature,
                                                  const sl::ViewportHandle& viewport)
{
    LOG_FUNC();
    auto result = o_slAllocateResources(cmdBuffer, feature, viewport);
    return result;
}

sl::Result StreamlineHooks::hkslGetNativeInterface(void* proxyInterface, void** baseInterface)
{
    LOG_FUNC();
    auto result = o_slGetNativeInterface(proxyInterface, baseInterface);
    return result;
}

sl::Result StreamlineHooks::hkslSetD3DDevice(void* d3dDevice)
{
    LOG_FUNC();
    auto result = o_slSetD3DDevice(d3dDevice);
    return result;
}

void StreamlineHooks::streamlineLogCallback_sl1(sl1::LogType type, const char* msg)
{
    if (msg == nullptr)
        return;

    char* trimmed_msg = trimStreamlineLog(msg);

    if (trimmed_msg != nullptr)
    {
        switch (type)
        {
        case sl1::LogType::eLogTypeWarn:
            LOG_WARN("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeInfo:
            // Same as sl2: SL info is noisy; see streamlineLogCallback.
            LOG_DEBUG("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeError:
            LOG_ERROR("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeCount:
            LOG_ERROR("{}", trimmed_msg);
            break;
        }

        free(trimmed_msg);
    }

    if (o_logCallback_sl1)
        o_logCallback_sl1(type, msg);
}

bool StreamlineHooks::hkslInit_sl1(const sl1::Preferences& pref, int applicationId)
{
    LOG_FUNC();

    sl1::Preferences localPref = pref;

    if (localPref.logMessageCallback != &streamlineLogCallback_sl1)
        o_logCallback_sl1 = localPref.logMessageCallback;
    localPref.logLevel = sl1::LogLevel::eLogLevelCount;
    localPref.logMessageCallback = &streamlineLogCallback_sl1;
    return o_slInit_sl1(localPref, applicationId);
}

bool StreamlineHooks::hkslSetTag_sl1(const sl1::Resource* resource, sl1::BufferType tag, uint32_t id,
                                     const sl1::Extent* extent)
{
    if (IsSL1AndFGActive())
        State::Instance().s_sl1FGInputs.setTag(resource, tag, id, extent);

    return o_slSetTag_sl1(resource, tag, id, extent);
}

bool StreamlineHooks::hkslSetConstants_sl1(const sl1::Constants& values, uint32_t frameIndex, uint32_t id)
{
    std::scoped_lock lock(setConstantsMutex);

    LOG_TRACE("SL1 slSetConstants frameIndex: {}, id: {}", frameIndex, id);

    if (IsSL1AndFGActive())
        State::Instance().s_sl1FGInputs.setConstants(values, frameIndex, id);

    return o_slSetConstants_interposer_sl1(values, frameIndex, id);
}

bool StreamlineHooks::hkslEvaluateFeature_sl1(sl1::CommandBuffer* cmdBuffer, sl1::Feature feature, uint32_t frameIndex,
                                              uint32_t id)
{
    LOG_TRACE("SL1 slEvaluateFeature feature: {}, frameIndex: {}, id: {}", magic_enum::enum_name(feature), frameIndex,
              id);

    if (IsSL1AndFGActive() && feature == sl1::Feature::eFeatureReflex)
    {
        const auto marker = (sl1::ReflexMarker) id;

        if (marker == sl1::ReflexMarker::eReflexMarkerRenderSubmitStart)
        {
            State::Instance().s_sl1FGInputs.evaluateState();
            State::Instance().s_sl1FGInputs.evaluateFeature(cmdBuffer, feature, frameIndex, id);
        }
        else if (marker == sl1::ReflexMarker::eReflexMarkerRenderSubmitEnd)
        {
            // Complete the resources replayed at RenderSubmitStart without replaying them a second time.
            State::Instance().s_sl1FGInputs.ReportEvaluate(frameIndex);
        }
        else if (marker == sl1::ReflexMarker::eReflexMarkerPresentStart)
        {
            State::Instance().s_sl1FGInputs.markPresent(frameIndex);
        }
    }

    return o_slEvaluateFeature_sl1(cmdBuffer, feature, frameIndex, id);
}

// Always re-read from params: the caps struct belongs to the sl.common copy Streamline currently uses,
// which can change (game plugins vs OTA plugins), so a cached pointer can go stale.
void StreamlineHooks::hookSystemCaps(sl::param::IParameters* params)
{
    if (params == nullptr)
        return;

    if (State::Instance().streamlineVersion.major > 1)
    {
        SystemCaps* caps = nullptr;
        sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &caps);

        if (caps != nullptr && !IsWritableMemory(caps, sizeof(SystemCaps)))
        {
            LOG_WARN("SystemCaps pointer {:X} is not valid memory, skipping arch spoofing", (size_t) caps);
            caps = nullptr;
        }

        systemCaps = caps;
    }
    else if (State::Instance().streamlineVersion.major == 1)
    {
        // This should be Streamline 1.5 as previous versions don't even have slOnPluginLoad
        LOG_TRACE("Attempting to get system caps for Streamline v1, this could fail depending on the exact version");

        SystemCapsSl15* caps = nullptr;
        sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &caps);

        if (caps != nullptr && !IsWritableMemory(caps, sizeof(SystemCapsSl15)))
        {
            LOG_WARN("SystemCaps (SL1) pointer {:X} is not valid memory, skipping arch spoofing", (size_t) caps);
            caps = nullptr;
        }

        systemCapsSl15 = caps;
    }
}

uint32_t StreamlineHooks::getSystemCapsArch(SystemCaps* altSystemCaps)
{
    uint32_t highestArch = 0;

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (!fakenvapi::isUsingAsMainNvapi() && primaryGpu.vendorId == VendorId::Nvidia)
    {
        if (State::Instance().streamlineVersion.major > 1)
        {
            auto caps = altSystemCaps != nullptr ? altSystemCaps : systemCaps;
            if (caps && IsWritableMemory(caps, sizeof(SystemCaps)))
            {
                for (auto& adapter : caps->adapters)
                {
                    if (adapter.architecture > highestArch)
                        highestArch = adapter.architecture;
                }
            }
        }
        else if (State::Instance().streamlineVersion.major == 1)
        {
            if (systemCapsSl15 && IsWritableMemory(systemCapsSl15, sizeof(SystemCapsSl15)))
            {
                const auto count = (std::min) (systemCapsSl15->gpuCount, kMaxNumSupportedGPUs);
                for (uint32_t i = 0; i < count; i++)
                {
                    if (systemCapsSl15->architecture[i] > highestArch)
                        highestArch = systemCapsSl15->architecture[i];
                }
            }
        }
    }

    // By default spoof Pascal, gets Reflex but not DLSSD
    // Could be problematic if not using fakenvapi but nvapi might not be initialized yet
    if (highestArch == 0)
        highestArch = NV_GPU_ARCHITECTURE_GP100;

    return highestArch;
}

void StreamlineHooks::setArch(uint32_t arch, SystemCaps* altSystemCaps)
{
    auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    // altSystemCaps has to be sl2+
    if (State::Instance().streamlineVersion.major > 1 || altSystemCaps)
    {
        // Assumes that altCaps are always for SL2+
        auto caps = altSystemCaps != nullptr ? altSystemCaps : systemCaps;
        if (caps && !IsWritableMemory(caps, sizeof(SystemCaps)))
        {
            LOG_WARN("SystemCaps at {:X} is no longer valid, not writing arch", (size_t) caps);

            if (caps == systemCaps)
                systemCaps = nullptr;

            caps = nullptr;
        }

        if (caps)
        {
            const auto count = (std::min) (caps->gpuCount, kMaxNumSupportedGPUs);
            for (uint32_t i = 0; i < count; i++)
            {
                caps->adapters[i].architecture = arch;
                caps->adapters[i].vendor = VendorId::Nvidia;
            }

            if (fakenvapi::isUsingAsMainNvapi() || primaryGpu.vendorId != VendorId::Nvidia)
                caps->driverVersionMajor = 999;

            caps->hwsSupported = true;
        }
    }
    else if (State::Instance().streamlineVersion.major == 1)
    {
        if (systemCapsSl15 && !IsWritableMemory(systemCapsSl15, sizeof(SystemCapsSl15)))
        {
            LOG_WARN("SystemCaps (SL1) at {:X} is no longer valid, not writing arch", (size_t) systemCapsSl15);
            systemCapsSl15 = nullptr;
        }

        if (systemCapsSl15)
        {
            const auto count = (std::min) (systemCapsSl15->gpuCount, kMaxNumSupportedGPUs);
            for (uint32_t i = 0; i < count; i++)
                systemCapsSl15->architecture[i] = arch;

            if (fakenvapi::isUsingAsMainNvapi() || primaryGpu.vendorId != VendorId::Nvidia)
                systemCapsSl15->driverVersionMajor = 999;

            systemCapsSl15->hwSchedulingEnabled = true;
        }
    }
}

// Spoof arch based on feature and current arch
void StreamlineHooks::spoofArch(uint32_t currentArch, sl::Feature feature, SystemCaps* altSystemCaps)
{
    constexpr uint32_t maxArch = 0xFFFFFFFF;

    // Don't change arch for DLSS/DLSSD with turing and above
    if (feature == sl::kFeatureDLSS)
    {
        if (currentArch < NV_GPU_ARCHITECTURE_TU100)
            return setArch(maxArch, altSystemCaps);
    }

    // Don't spoof DLSSD at all
    else if (feature == sl::kFeatureDLSS_RR)
    {
        return;
    }

    // Don't change arch for DLSSG with ada and above
    else if (feature == sl::kFeatureDLSS_G)
    {
        if (State::Instance().activeFgNvngx != FGNvngxReplacement::None)
        {
            if (!Nvngx_FG::isDx12Available() && !Nvngx_FG::isVulkanAvailable())
                return setArch(0);
        }

        if (currentArch < NV_GPU_ARCHITECTURE_AD100)
            return setArch(maxArch, altSystemCaps);
    }

    else if (feature == sl::kFeatureReflex || feature == sl::kFeaturePCL)
    {
        if (fakenvapi::isUsingAsMainNvapi())
            return setArch(maxArch, altSystemCaps);
    }
}

bool StreamlineHooks::hkdlss_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                            const char** pluginJSON)
{
    // Plugin initialization may run under the loader. Never wait for another
    // thread's architecture/JSON patch while holding the Windows loader lock.
    std::unique_lock configLock(pluginConfigMutex, std::try_to_lock);
    if (!configLock) return false;

    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    auto& config = Calls().config;

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureDLSS);
    }

    auto result = Calls().o_dlss_slOnPluginLoad.load()(params, loaderJSON, pluginJSON);

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    if (!result || !pluginJSON || !*pluginJSON) return result;

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON, nullptr, false);
    if (configJson.is_discarded())
    {
        LOG_ERROR("Failed to parse plugin JSON");
        return result;
    }

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslDLSSGetOptimalSettings(const sl::DLSSOptions& options,
                                                       sl::DLSSOptimalSettings& settings)
{
    auto& modesBroken = Calls().modesBroken;

    auto localOptions = options;

    if (localOptions.mode == sl::DLSSMode::eOff)
        modesBroken = true;

    if (modesBroken)
    {
        if (localOptions.mode == sl::DLSSMode::eMaxPerformance)
            localOptions.mode = sl::DLSSMode::eUltraPerformance;
        else if (localOptions.mode == sl::DLSSMode::eBalanced)
            localOptions.mode = sl::DLSSMode::eMaxPerformance;
        else if (localOptions.mode == sl::DLSSMode::eMaxQuality)
            localOptions.mode = sl::DLSSMode::eBalanced;
        else if (localOptions.mode == sl::DLSSMode::eUltraQuality)
            localOptions.mode = sl::DLSSMode::eMaxQuality;
        else if (localOptions.mode == sl::DLSSMode::eUltraPerformance)
            localOptions.mode = sl::DLSSMode::eDLAA;
    }

    return Calls().o_slDLSSGetOptimalSettings.load()(localOptions, settings);
}

bool StreamlineHooks::hkdlssg_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                             const char** pluginJSON)
{
    // Plugin initialization may run under the loader. Never wait for another
    // thread's architecture/JSON patch while holding the Windows loader lock.
    std::unique_lock configLock(pluginConfigMutex, std::try_to_lock);
    if (!configLock) return false;

    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    auto& config = Calls().config;

    bool shouldSpoofArch =
        Config::Instance()->StreamlineSpoofing.value_or_default() &&
        (State::Instance().activeFgInput == FGInput::NvngxFG || State::Instance().activeFgInput == FGInput::DLSSG);

    uint32_t currentArch = 0;
    if (shouldSpoofArch)
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureDLSS_G);
    }

    auto result = Calls().o_dlssg_slOnPluginLoad.load()(params, loaderJSON, pluginJSON);

    if (shouldSpoofArch)
        setArch(currentArch);

    if (!result || !pluginJSON || !*pluginJSON) return result;

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON, nullptr, false);
    if (configJson.is_discarded())
    {
        LOG_ERROR("Failed to parse plugin JSON");
        return result;
    }

    // Kill the DLSSG streamline swapchain hooks
    if (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgOutput == FGOutput::DLSSG)
    {
        if (configJson.contains("/hooks"_json_pointer))
            configJson["hooks"].clear();

        if (configJson.contains("/exclusive_hooks"_json_pointer))
            configJson["exclusive_hooks"].clear();

        if (configJson.contains("/external/feature/tags"_json_pointer))
            configJson["external"]["feature"]["tags"].clear(); // We handle the DLSSG resources

        if (configJson.contains("/external/vk/device/queues/compute/count"_json_pointer))
            configJson["external"]["vk"]["device"]["queues"]["compute"]["count"] = 0;

        if (configJson.contains("/external/vk/device/queues/graphics/count"_json_pointer))
            configJson["external"]["vk"]["device"]["queues"]["graphics"]["count"] = 0;

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgInput == FGInput::NvngxFG)
    {
        if (configJson.contains("/vsync/supported"_json_pointer))
            configJson["vsync"]["supported"] = true; // disable eVSyncOffRequired

        if (configJson.contains("/external/hws/required"_json_pointer))
            configJson["external"]["hws"]["required"] = false; // disable eHardwareSchedulingRequired

        // if (configJson.contains("/external/vk/opticalflow/supported"_json_pointer))
        //     configJson["external"]["vk"]["opticalflow"]["supported"] = true;
    }

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

const char* StreamlineHooks::hkdlssg_slGetPluginJSONConfig_sl1()
{
    // Plugin initialization may run under the loader. Never wait for another
    // thread's architecture/JSON patch while holding the Windows loader lock.
    std::unique_lock configLock(pluginConfigMutex, std::try_to_lock);
    if (!configLock) return nullptr;

    auto& patchedConfig = Calls().jsonConfig;

    const char* originalConfig = Calls().o_dlssg_slGetPluginJSONConfig_sl1.load()();

    if (originalConfig == nullptr)
        return originalConfig;

    try
    {
        auto configJson = nlohmann::json::parse(originalConfig);

        LOG_DEBUG("SL1 DLSSG JSON before patch: {}", configJson.dump());

        PatchSL1PluginJson(configJson);
        // RemoveSL1DLSSGHookEntriesRecursive(configJson);

        patchedConfig = configJson.dump();

        LOG_DEBUG("SL1 DLSSG JSON after patch: {}", patchedConfig);

        return patchedConfig.c_str();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to patch SL1 DLSSG JSON config: {}", e.what());
        return originalConfig;
    }
}

bool StreamlineHooks::hklocal_dlssg_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                                   const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    bool shouldSpoofArch = Config::Instance()->StreamlineSpoofing.value_or_default();

    uint32_t currentArch = 0;
    SystemCaps* localSystemCaps = nullptr;
    if (shouldSpoofArch)
    {
        sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &localSystemCaps);

        if (localSystemCaps)
        {
            currentArch = getSystemCapsArch(localSystemCaps);
            spoofArch(currentArch, sl::kFeatureDLSS_G, localSystemCaps);
        }
    }

    PinnedCall localDlssgOnLoad(localDlssgModules, localDlssgOnLoadFn);
    auto result = localDlssgOnLoad ? localDlssgOnLoad.fn(params, loaderJSON, pluginJSON) : false;

    if (shouldSpoofArch && localSystemCaps)
        setArch(currentArch, localSystemCaps);

    if (!HasPluginJson(result, pluginJSON))
        return result;

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON, nullptr, false);
    if (configJson.is_discarded())
    {
        LOG_ERROR("Failed to parse plugin JSON");
        return result;
    }

    if (configJson.contains("/external/hws/required"_json_pointer))
        configJson["external"]["hws"]["required"] = false; // disable eHardwareSchedulingRequired

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslSetConstants(const sl::Constants& values, const sl::FrameToken& frame,
                                             const sl::ViewportHandle& viewport)
{
    std::scoped_lock lock(setConstantsMutex);
    LOG_TRACE("called with frameIndex: {}, viewport: {}", (unsigned int) frame, (unsigned int) viewport);

    State::Instance().slFGInputs.setConstants(values, (uint32_t) frame);
    State::Instance().slLastConstants = values;
    State::Instance().slLastConstantsFrame = static_cast<uint32_t>(frame);
    State::Instance().slLastConstantsViewport = static_cast<uint32_t>(viewport);

    return o_slSetConstants(values, frame, viewport);
}

bool StreamlineHooks::isSetConstantsHooked() { return o_slSetConstants != nullptr; }

bool StreamlineHooks::hkcommon_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                              const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    PinnedCall commonOnLoad(commonModules, commonOnLoadFn);
    auto result = commonOnLoad ? commonOnLoad.fn(params, loaderJSON, pluginJSON) : false;
    if (!HasPluginJson(result, pluginJSON))
        return result;

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON, nullptr, false);
    if (configJson.is_discarded())
    {
        LOG_ERROR("Failed to parse plugin JSON");
        return result;
    }
    auto& slVersion = State::Instance().streamlineVersion;

    // Grab a version of the potentially updated sl.common
    // Opti assumes that all plugins will have this version
    configJson.at("version").at("major").get_to(slVersion.major);
    configJson.at("version").at("minor").get_to(slVersion.minor);
    configJson.at("version").at("build").get_to(slVersion.patch);

    // Completely disables Streamline hooks
    // if (true)
    //    configJson["hooks"].clear();
    //    configJson["exclusive_hooks"].clear();
    //}

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslDLSSGSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    ReportStreamlineFGRequest(options);

    if (Calls().o_slDLSSGSetOptions.load() == nullptr)
        return sl::Result::eErrorFeatureMissing;

    activeDlssg.store(currentPlugin);
    {
        std::lock_guard lock(Calls().optionsMutex);
        Calls().viewport = viewport;
        Calls().options = options;
    }

    // Avoid reading past the game's struct's size
    sl::DLSSGOptions newOptions {};
    auto newStructVer = newOptions.structVersion;

    if (options.structVersion == 1)
        memcpy(&newOptions, &options, 104);
    else if (options.structVersion == 2 || options.structVersion == 3)
        memcpy(&newOptions, &options, 112);
    else if (options.structVersion == 4 || options.structVersion == 5)
        memcpy(&newOptions, &options, 120);
    else
        newOptions = options;

    newOptions.structVersion = newStructVer;

    auto& state = State::Instance();

    // Disable game's DLSSG when we are trying to create our own instance of DLSSG
    if (state.activeFgInput != FGInput::DLSSG && state.activeFgOutput == FGOutput::DLSSG)
    {
        newOptions.mode = sl::DLSSGMode::eOff;
        return Calls().o_slDLSSGSetOptions.load()(viewport, newOptions);
    }

    // Make DLSSG auto always mean On
    if (newOptions.mode == sl::DLSSGMode::eAuto)
        newOptions.mode = sl::DLSSGMode::eOn;

    const auto dlssgPotentiallyActive = newOptions.mode == sl::DLSSGMode::eOn ||
                                        newOptions.mode == sl::DLSSGMode::eAuto ||
                                        newOptions.mode == sl::DLSSGMode::eDynamic;

    bool enableDynamicMode = Config::Instance()->FGDLSSGOverrideForceDMFG.value_or_default() &&
                             state.dlssgGameDMFGSupported && dlssgPotentiallyActive;

    if (enableDynamicMode)
    {
        newOptions.mode = sl::DLSSGMode::eDynamic;
    }

    if (newOptions.mode == sl::DLSSGMode::eDynamic && Config::Instance()->FGDLSSGFramerateTargetDMFG.has_value())
    {
        newOptions.dynamicTargetFrameRate = Config::Instance()->FGDLSSGFramerateTargetDMFG.value();
    }

    if (state.swapchainApi == API::Vulkan)
    {
        // Only matters for Vulkan, DX doesn't use this delay
        if (dlssgPotentiallyActive && !MenuOverlayBase::IsVisible())
            state.delayMenuRenderBy = 10;

        if (MenuOverlayBase::IsVisible())
        {
            newOptions.mode = sl::DLSSGMode::eOff;
            newOptions.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
            ReflexHooks::setDlssgFrameCount(0);
        }
    }

    LOG_TRACE("DLSSG Modified Mode: {}", magic_enum::enum_name(newOptions.mode));

    if (dlssgPotentiallyActive && state.streamlineVersion >= feature_version { 2, 7, 1 })
    {
        // Populate dlssgMfgMax once
        if (!state.dlssgMfgMax.has_value())
        {
            sl::DLSSGState localState {};
            sl::DLSSGOptions localOptions {};
            if (Calls().o_slDLSSGGetState.load() &&
                Calls().o_slDLSSGGetState.load()(viewport, localState, &localOptions) == sl::Result::eOk &&
                localState.numFramesToGenerateMax > 0 && localState.numFramesToGenerateMax < 6)
            {
                state.dlssgMfgMax = localState.numFramesToGenerateMax;
                LOG_TRACE("Saving original numFramesToGenerateMax: {}", state.dlssgMfgMax.value());

                if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value() &&
                    Config::Instance()->FGDLSSGOverrideInterpolationCount.value() > state.dlssgMfgMax.value())
                {
                    Config::Instance()->FGDLSSGOverrideInterpolationCount.set_volatile_value(state.dlssgMfgMax.value());
                }
            }
        }

        // Won't take effect with Dynamic
        if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value())
        {
            auto overrideCount = Config::Instance()->FGDLSSGOverrideInterpolationCount.value();
            if (overrideCount != 0)
                newOptions.numFramesToGenerate = overrideCount;
            else if (!enableDynamicMode)
                newOptions.mode = sl::DLSSGMode::eOff;
        }
    }

    state.dlssgLastSetMode = newOptions.mode;

    const auto result = Calls().o_slDLSSGSetOptions.load()(viewport, newOptions);
    static thread_local int loggedMode = -1;
    static thread_local uint32_t loggedCount = ~0u;
    if (loggedMode != static_cast<int>(newOptions.mode) || loggedCount != newOptions.numFramesToGenerate ||
        result != sl::Result::eOk)
    {
        LOG_INFO("DLSSG options: requested={} forwarded={} generatedFrames={} result={}",
                 static_cast<int>(options.mode), static_cast<int>(newOptions.mode),
                 newOptions.numFramesToGenerate, static_cast<int>(result));
        loggedMode = static_cast<int>(newOptions.mode);
        loggedCount = newOptions.numFramesToGenerate;
    }
    return result;
}

sl::Result StreamlineHooks::hkslDLSSGGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                              const sl::DLSSGOptions* options)
{
    if (Calls().o_slDLSSGGetState.load() == nullptr)
        return sl::Result::eErrorFeatureMissing;

    sl::Result result {};

    const auto originalStructVersion = state.structVersion;
    if (originalStructVersion < 4)
    {
        sl::DLSSGState newState {};

        // We might be feeding a newer struct to an older SL but that seems to work just fine for this Get function
        result = Calls().o_slDLSSGGetState.load()(viewport, dynamic_cast<sl::DLSSGState&>(newState), options);
        if(result != sl::Result::eOk) return result;

        // Copy back data to game's struct
        memcpy(&state, &newState, 56); // struct ver 1 size
        state.structVersion = originalStructVersion;

        if (originalStructVersion >= 2)
        {
            state.numFramesToGenerateMax = newState.numFramesToGenerateMax;
            state.bReserved4 = newState.bReserved4;
            state.bIsVsyncSupportAvailable = newState.bIsVsyncSupportAvailable;
        }

        if (originalStructVersion >= 3)
        {
            state.inputsProcessingCompletionFence = newState.inputsProcessingCompletionFence;
            state.lastPresentInputsProcessingCompletionFenceValue =
                newState.lastPresentInputsProcessingCompletionFenceValue;
        }

        State::Instance().dlssgGameDMFGSupported = newState.bIsDynamicMFGSupported == sl::eTrue;
    }
    else
    {
        result = Calls().o_slDLSSGGetState.load()(viewport, state, options);
        if(result != sl::Result::eOk) return result;
        State::Instance().dlssgGameDMFGSupported = state.bIsDynamicMFGSupported == sl::eTrue;
    }

    if (!State::Instance().dlssgGameDMFGSupported)
    {
        Config::Instance()->FGDLSSGOverrideForceDMFG.set_volatile_value(false);
    }

    auto& optiState = State::Instance();

    if (optiState.streamlineVersion >= feature_version { 2, 7, 1 })
    {
        if (!optiState.dlssgMfgMax.has_value())
        {
            sl::DLSSGState localState {};
            sl::DLSSGOptions localOptions {};
            if (Calls().o_slDLSSGGetState.load() &&
                Calls().o_slDLSSGGetState.load()(viewport, localState, &localOptions) == sl::Result::eOk &&
                localState.numFramesToGenerateMax > 0 && localState.numFramesToGenerateMax < 6)
            {
                optiState.dlssgMfgMax = localState.numFramesToGenerateMax;
                LOG_TRACE("Saving original numFramesToGenerateMax: {}", optiState.dlssgMfgMax.value());

                if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value() &&
                    Config::Instance()->FGDLSSGOverrideInterpolationCount.value() > optiState.dlssgMfgMax.value())
                {
                    Config::Instance()->FGDLSSGOverrideInterpolationCount.set_volatile_value(optiState.dlssgMfgMax.value());
                }
            }
        }
    }

    if (optiState.activeFgInput == FGInput::DLSSG)
    {
        if (optiState.currentFG != nullptr && options != nullptr &&
            options->flags & sl::DLSSGFlags::eRequestVRAMEstimate)
            state.estimatedVRAMUsageInBytes = static_cast<uint64_t>(256 * 1024) * 1024;

        FillDlssgInputState(state);

        LOG_DEBUG("Status: {}, numFramesActuallyPresented: {}", magic_enum::enum_name(state.status),
                  state.numFramesActuallyPresented);
    }

    return result;
}

bool StreamlineHooks::hkreflex_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                              const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureReflex);
    }

    PinnedCall reflexOnLoad(reflexModules, reflexOnLoadFn);
    auto result = reflexOnLoad ? reflexOnLoad.fn(params, loaderJSON, pluginJSON) : false;

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    if (!HasPluginJson(result, pluginJSON))
        return result;

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON, nullptr, false);
    if (configJson.is_discarded())
    {
        LOG_ERROR("Failed to parse plugin JSON");
        return result;
    }

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslReflexSetOptions(const sl::ReflexOptions& options)
{
    PinnedCall setOptions(reflexModules, reflexSetOptionsFn);
    if (!setOptions)
        return sl::Result::eErrorFeatureMissing;

    reflexGamesLastMode = options.mode;

    sl::ReflexOptions newOptions = options;

    if (Config::Instance()->FN_ForceReflex == ForceReflex::ForceEnable)
        newOptions.mode = sl::ReflexMode::eLowLatencyWithBoost;

    // Will cause a pink screen when used with DLSSG
    // if (Config::Instance()->FN_ForceReflex == 1)
    //     newOptions.mode = sl::ReflexMode::eOff;

    return setOptions.fn(newOptions);
}

sl::Result StreamlineHooks::hkslReflexSleep(const sl::FrameToken& frame)
{
    PinnedCall sleep(reflexModules, reflexSleepFn);
    if (!sleep)
        return sl::Result::eErrorFeatureMissing;

    // if (State::Instance().activeFgOutput == FGOutput::DLSSG && StreamlineProxy::IsD3D12Inited() &&
    //     Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default())
    //{
    //     return StreamlineProxy::ReflexSleep()(frame);
    // }

    return sleep.fn(frame);
}

void* StreamlineHooks::hkdlss_slGetPluginFunction(const char* functionName)
{
    if (!functionName) return nullptr;
    LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        Calls().o_dlss_slOnPluginLoad.store((PFN_slOnPluginLoad) Calls().o_dlss_slGetPluginFunction.load()(functionName));
        if (!Calls().o_dlss_slOnPluginLoad.load()) return nullptr;
        return Callback<&hkdlss_slOnPluginLoad>(currentPlugin);
    }

    if (strcmp(functionName, "slDLSSGetOptimalSettings") == 0 &&
        State::Instance().gameQuirks & GameQuirk::PregmataFixDLSSModes)
    {
        Calls().o_slDLSSGetOptimalSettings.store((decltype(&slDLSSGetOptimalSettings)) Calls().o_dlss_slGetPluginFunction.load()(functionName));
        if (!Calls().o_slDLSSGetOptimalSettings.load()) return nullptr;
        return Callback<&hkslDLSSGetOptimalSettings>(currentPlugin);
    }

    return Calls().o_dlss_slGetPluginFunction.load()(functionName);
}

void* StreamlineHooks::hkdlssg_slGetPluginFunction(const char* functionName)
{
    if (!functionName) return nullptr;
    // Resolve the companion call from this same module.
    if (!Calls().o_slDLSSGGetState.load())
        Calls().o_slDLSSGGetState.store((decltype(&slDLSSGGetState)) Calls().o_dlssg_slGetPluginFunction.load()("slDLSSGGetState"));


    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        Calls().o_dlssg_slOnPluginLoad.store((PFN_slOnPluginLoad) Calls().o_dlssg_slGetPluginFunction.load()(functionName));
        if (!Calls().o_dlssg_slOnPluginLoad.load()) return nullptr;
        return Callback<&hkdlssg_slOnPluginLoad>(currentPlugin);
    }

    if (strcmp(functionName, "slDLSSGSetOptions") == 0)
    {
        Calls().o_slDLSSGSetOptions.store((decltype(&slDLSSGSetOptions)) Calls().o_dlssg_slGetPluginFunction.load()(functionName));

        // Give steam overlay the original as it seems to be hooking it
        auto steamOverlay = KernelBaseProxy::GetModuleHandleA_()("gameoverlayrenderer64.dll");
        if (steamOverlay != nullptr)
        {
            if (HMODULE callerModule = Util::GetCallerModule(pluginCaller); callerModule == steamOverlay)
                return Calls().o_slDLSSGSetOptions.load();
        }

        if (!Calls().o_slDLSSGSetOptions.load()) return nullptr;
        return Callback<&hkslDLSSGSetOptions>(currentPlugin);
    }

    if (strcmp(functionName, "slDLSSGGetState") == 0)
    {
        Calls().o_slDLSSGGetState.store((decltype(&slDLSSGGetState)) Calls().o_dlssg_slGetPluginFunction.load()(functionName));

        // Give steam overlay the original as it seems to be hooking it
        auto steamOverlay = KernelBaseProxy::GetModuleHandleA_()("gameoverlayrenderer64.dll");
        if (steamOverlay != nullptr)
        {
            if (HMODULE callerModule = Util::GetCallerModule(pluginCaller); callerModule == steamOverlay)
                return Calls().o_slDLSSGGetState.load();
        }

        if (!Calls().o_slDLSSGGetState.load()) return nullptr;
        return Callback<&hkslDLSSGGetState>(currentPlugin);
    }

    if (strcmp(functionName, "slGetPluginJSONConfig") == 0 && IsSL1AndDLSSGManaged())
    {
        Calls().o_dlssg_slGetPluginJSONConfig_sl1.store(
            reinterpret_cast<PFN_slGetPluginJSONConfig_sl1>(Calls().o_dlssg_slGetPluginFunction.load()(functionName)));

        if (Calls().o_dlssg_slGetPluginJSONConfig_sl1.load() != nullptr)
        {
            LOG_WARN("Hooking SL1 DLSSG slGetPluginJSONConfig");
            return Callback<&hkdlssg_slGetPluginJSONConfig_sl1>(currentPlugin);
        }
    }

    // Ensure that we have those DLSSG calls
    if (!Calls().o_slDLSSGSetOptions.load())
        Calls().o_slDLSSGSetOptions.store((decltype(&slDLSSGSetOptions)) Calls().o_dlssg_slGetPluginFunction.load()("slDLSSGSetOptions"));

    if (!Calls().o_slDLSSGGetState.load())
        Calls().o_slDLSSGGetState.store((decltype(&slDLSSGGetState)) Calls().o_dlssg_slGetPluginFunction.load()("slDLSSGGetState"));

    return Calls().o_dlssg_slGetPluginFunction.load()(functionName);
}

void* StreamlineHooks::hklocal_dlssg_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    // The thunk pins this slot's module for the resolution
    auto getFn = localDlssgGetFn[currentLocalDlssgSlot].load();

    if (getFn == nullptr)
        return nullptr;

    if (strcmp(functionName, "slOnPluginLoad") == 0 && State::Instance().activeFgNvngx != FGNvngxReplacement::None)
    {
        localDlssgOnLoadFn[currentLocalDlssgSlot] = (PFN_slOnPluginLoad) getFn(functionName);
        return &hklocal_dlssg_slOnPluginLoad;
    }

    return getFn(functionName);
}

bool StreamlineHooks::hkreflex_slSetConstants_sl1(const void* data, uint32_t frameIndex, uint32_t id)
{
    PinnedCall setConstants(reflexModules, reflexSetConstantsSl1Fn);
    if (!setConstants)
        return false;

    // Streamline v1's version of slReflexSetOptions + slPCLSetMarker
    static sl1::ReflexConstants constants {};
    constants = *(const sl1::ReflexConstants*) data;

    reflexGamesLastMode = (sl::ReflexMode) constants.mode;

    LOG_DEBUG("mode: {}, frameIndex: {}, id: {}", (uint32_t) constants.mode, frameIndex, id);

    if (Config::Instance()->FN_ForceReflex == ForceReflex::ForceEnable)
        constants.mode = sl1::ReflexMode::eReflexModeLowLatencyWithBoost;

    // Will cause a pink screen when used with DLSSG
    // else if (Config::Instance()->FN_ForceReflex == 1)
    //     constants.mode = sl1::ReflexMode::eReflexModeOff;

    return setConstants.fn(&constants, frameIndex, id);
}

void* StreamlineHooks::hkreflex_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    // The thunk pins this slot's module for the resolution
    auto getFn = reflexGetFn[currentReflexSlot].load();

    if (getFn == nullptr)
        return nullptr;

    if (strcmp(functionName, "slSetConstants") == 0 && State::Instance().streamlineVersion.major == 1)
    {
        reflexSetConstantsSl1Fn[currentReflexSlot] = (PFN_slSetConstants_sl1) getFn(functionName);
        return &hkreflex_slSetConstants_sl1;
    }

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        reflexOnLoadFn[currentReflexSlot] = (PFN_slOnPluginLoad) getFn(functionName);
        return &hkreflex_slOnPluginLoad;
    }

    if (strcmp(functionName, "slReflexSetOptions") == 0)
    {
        reflexSetOptionsFn[currentReflexSlot] = (decltype(&slReflexSetOptions)) getFn(functionName);
        return &hkslReflexSetOptions;
    }

    if (strcmp(functionName, "slReflexSleep") == 0)
    {
        reflexSleepFn[currentReflexSlot] = (decltype(&slReflexSleep)) getFn(functionName);
        return &hkslReflexSleep;
    }

    // TODO: Hopefully a game doesn't call both, maybe separate
    if (strcmp(functionName, "slReflexSetMarker") == 0 &&
        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||
         State::Instance().activeFgInput == FGInput::DLSSG))
    {
        reflexSetMarkerFn[currentReflexSlot] = (decltype(&slPCLSetMarker)) getFn(functionName);
        return &hkslPCLSetMarker;
    }

    return getFn(functionName);
}

sl::Result StreamlineHooks::hkslPCLSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame)
{
    // Resolved from sl.pcl, or with the FixSlSimulationMarkers quirk / DLSSG input from sl.reflex
    PinnedCall pclMarker(pclModules, pclSetMarkerFn);
    PinnedCall reflexMarker(reflexModules, reflexSetMarkerFn);
    auto original = pclMarker ? pclMarker.fn : reflexMarker.fn;

    if (original == nullptr)
        return sl::Result::eErrorFeatureMissing;


    // Indiana Jones sends a nullptr on startup :)
    const void* volatile framePtr = &frame;
    if (framePtr == nullptr)
    {
        LOG_WARN("Null frame token for marker {}", (uint32_t) marker);
        return original(marker, frame);
    }

    // if (State::Instance().activeFgOutput == FGOutput::DLSSG && StreamlineProxy::IsD3D12Inited() &&
    //     Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default())
    //{
    //     return StreamlineProxy::PCLSetMarker()(marker, frame);
    // }

    // HACK for broken games
    if (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers)
    {
        static uint64_t last_simulation_end_id = 0;
        if (marker == sl::PCLMarker::eSimulationEnd)
        {
            last_simulation_end_id = frame;
        }

        if (marker == sl::PCLMarker::eSimulationStart && last_simulation_end_id >= frame && o_slGetNewFrameToken)
        {
            const uint64_t correction_offset = last_simulation_end_id - frame + 1;
            uint32_t newFrameId = static_cast<uint32_t>(frame + correction_offset);

            sl::FrameToken* newFramePointer {};
            auto result = o_slGetNewFrameToken(newFramePointer, &newFrameId);

            LOG_WARN("Simulation start marker sent after end marker, offset: {}", correction_offset);
            InputCollection::getInstance().startCollectingForFrame(newFrameId);

            result = original(marker, *newFramePointer);
            return result;
        }
    }

    if (marker == sl::PCLMarker::eSimulationStart)
        InputCollection::getInstance().startCollectingForFrame(frame);

    if (State::Instance().activeFgInput == FGInput::DLSSG)
    {
        if (State::Instance().streamlineVersion.major == 1)
        {
            if (marker == sl::PCLMarker::eRenderSubmitStart)
            {
                State::Instance().s_sl1FGInputs.evaluateState();
            }
            else if (marker == sl::PCLMarker::eRenderSubmitEnd)
            {
                State::Instance().s_sl1FGInputs.ReportEvaluate(static_cast<uint32_t>(frame));
            }
            else if (marker == sl::PCLMarker::ePresentStart)
            {
                State::Instance().s_sl1FGInputs.markPresent(frame);
            }
        }
        else
        {
            if (marker == sl::PCLMarker::eRenderSubmitStart)
            {
                State::Instance().slFGInputs.evaluateState();
            }
            else if (marker == sl::PCLMarker::ePresentStart)
            {
                State::Instance().slFGInputs.markPresent(frame);
            }
        }
    }

    return original(marker, frame);
}

bool StreamlineHooks::hkpcl_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                           const char** pluginJSON)
{
    LOG_FUNC();

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeaturePCL);
    }

    PinnedCall pclOnLoad(pclModules, pclOnLoadFn);
    auto result = pclOnLoad ? pclOnLoad.fn(params, loaderJSON, pluginJSON) : false;

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    return result;
}

void* StreamlineHooks::hkpcl_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    // The thunk pins this slot's module for the resolution
    auto getFn = pclGetFn[currentPclSlot].load();

    if (getFn == nullptr)
        return nullptr;

    if (strcmp(functionName, "slPCLSetMarker") == 0 &&
        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||
         State::Instance().activeFgInput == FGInput::DLSSG))
    {
        pclSetMarkerFn[currentPclSlot] = (decltype(&slPCLSetMarker)) getFn(functionName);
        return &hkslPCLSetMarker;
    }

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        pclOnLoadFn[currentPclSlot] = (PFN_slOnPluginLoad) getFn(functionName);
        return &hkpcl_slOnPluginLoad;
    }

    return getFn(functionName);
}

bool StreamlineHooks::hk_setVoid(void* self, const char* key, void** value)
{
    // LOG_DEBUG("{}", key);

    if (strcmp(key, sl::param::common::kSystemCaps) == 0)
    {
        LOG_TRACE("Attempting to change system caps for Streamline v1, this could fail depending on the exact version");

        // SystemCapsSl15 is not entirely correct for Streamline 1.3
        // But we here only use the beginning that matches + extra
        auto caps = (SystemCapsSl15*) value;

        if (caps)
        {
            caps->gpuCount = 1;
            caps->architecture[0] = UINT_MAX;
            caps->driverVersionMajor = 999;

            // HAGS
            *((char*) value + 56) = (char) 0x01;
        }
    }

    PinnedCall setVoid(commonModules, commonSetVoidFn);
    if (!setVoid)
        return false;

    return setVoid.fn(self, key, value);
}

void StreamlineHooks::hkcommon_slSetParameters_sl1(void* params)
{
    LOG_FUNC();

    PinnedCall setParameters(commonModules, commonSetParametersSl1Fn);

    if (setParameters && commonSetVoidFn[setParameters.slot] == nullptr && params)
    {
        void** vtable = *(void***) params;

        // It's flipped, 0 -> set void*, 7 -> get void*
        auto original = (PFN_setVoid) vtable[0];

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (original != nullptr)
            DetourAttach(&(PVOID&) original, hk_setVoid);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook setVoid: {:X}", detourResult);
            original = nullptr;
        }

        commonSetVoidFn[setParameters.slot] = original;
    }

    if (!setParameters)
        return;

    setParameters.fn(params);
}

void* StreamlineHooks::hkcommon_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    // The thunk pins this slot's module for the resolution
    auto getFn = commonGetFn[currentCommonSlot].load();

    if (getFn == nullptr)
        return nullptr;

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        commonOnLoadFn[currentCommonSlot] = (PFN_slOnPluginLoad) getFn(functionName);
        return &hkcommon_slOnPluginLoad;
    }

    // Used around Streamline v1.3, as 1.5 doesn't seem to have it anymore
    if (strcmp(functionName, "slSetParameters") == 0)
    {
        commonSetParametersSl1Fn[currentCommonSlot] = (PFN_slSetParameters_sl1) getFn(functionName);
        return &hkcommon_slSetParameters_sl1;
    }

    return getFn(functionName);
}

void StreamlineHooks::updateForceReflex()
{
    // Not needed for Streamline v1 as slSetConstants is sent every frame
    PinnedCall setOptions(reflexModules, reflexSetOptionsFn);

    if (setOptions)
    {
        sl::ReflexOptions options;

        auto forceReflex = Config::Instance()->FN_ForceReflex.value_or_default();

        if (forceReflex == ForceReflex::ForceEnable)
            options.mode = sl::ReflexMode::eLowLatencyWithBoost;
        else if (forceReflex == ForceReflex::ForceDisable)
            options.mode = sl::ReflexMode::eOff;
        else if (forceReflex == ForceReflex::InGame)
            options.mode = reflexGamesLastMode;

        auto result = setOptions.fn(options);
        if (result != sl::Result::eOk)
        {
            LOG_WARN("Failed to update Reflex mode with error code: {} ({:X})", magic_enum::enum_name(result),
                     (UINT) result);
        }
    }
}

void StreamlineHooks::updateDlssgOptions()
{
    const auto index = activeDlssg.load();
    if (index < kPluginSlots && pluginCalls[index].o_slDLSSGSetOptions.load())
    {
        sl::ViewportHandle viewport;
        sl::DLSSGOptions options;
        {
            std::lock_guard lock(pluginCalls[index].optionsMutex);
            viewport = pluginCalls[index].viewport;
            options = pluginCalls[index].options;
        }
        Callback<&hkslDLSSGSetOptions>(index)(viewport, options);
    }
}

// SL INTERPOSER

void StreamlineHooks::unhookInterposer()
{
    LOG_FUNC();

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_slSetTag)
        DetourDetach(&(PVOID&) o_slSetTag, hkslSetTag);

    if (o_slSetTagForFrame)
        DetourDetach(&(PVOID&) o_slSetTagForFrame, hkslSetTagForFrame);

    if (o_slSetConstants)
        DetourDetach(&(PVOID&) o_slSetConstants, hkslSetConstants);

    if (o_slEvaluateFeature)
        DetourDetach(&(PVOID&) o_slEvaluateFeature, hkslEvaluateFeature);

    if (o_slInit)
        DetourDetach(&(PVOID&) o_slInit, hkslInit);

    if (o_slInit_sl1)
        DetourDetach(&(PVOID&) o_slInit_sl1, hkslInit_sl1);

    if (o_slSetTag_sl1)
        DetourDetach(&(PVOID&) o_slSetTag_sl1, hkslSetTag_sl1);

    if (o_slSetConstants_interposer_sl1)
        DetourDetach(&(PVOID&) o_slSetConstants_interposer_sl1, hkslSetConstants_sl1);

    if (o_slEvaluateFeature_sl1)
        DetourDetach(&(PVOID&) o_slEvaluateFeature_sl1, hkslEvaluateFeature_sl1);

    // if (o_logCallback)
    //     DetourDetach(&(PVOID&) o_logCallback, streamlineLogCallback);
    // else if (o_logCallback_sl1)
    //     DetourDetach(&(PVOID&) o_logCallback_sl1, streamlineLogCallback);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("DetourTransactionCommit error: {:X}", detourResult);
    }
    else
    {
        o_slInit = nullptr;
        o_slInit_sl1 = nullptr;
        o_slSetTag = nullptr;
        o_slSetTagForFrame = nullptr;
        o_slEvaluateFeature = nullptr;
        o_slSetConstants = nullptr;
        o_slSetTag_sl1 = nullptr;
        o_slSetConstants_interposer_sl1 = nullptr;
        o_slEvaluateFeature_sl1 = nullptr;
        o_logCallback = nullptr;
        o_logCallback_sl1 = nullptr;
    }
}

// Call it just after sl.interposer's load or if sl.interposer is already loaded
void StreamlineHooks::hookInterposer(HMODULE slInterposer)
{
    if (State::Instance().externalFrameGeneration)
        return;
    LOG_FUNC();

    if (!slInterposer)
    {
        LOG_WARN("Streamline module in NULL");
        return;
    }

    // Interposer needs this or it might end in an infinite loop calling itself
    static HMODULE last_slInterposer = nullptr;

    if (last_slInterposer == slInterposer)
        return;

    last_slInterposer = slInterposer;

    // Looks like when reading DLL version load methods are called
    // To prevent loops disabling checks for sl.interposer.dll
    auto owner = State::GetOwner();
    State::DisableChecks(owner, "sl.interposer");

    if (o_slSetTag || o_slInit || o_slInit_sl1 || o_slSetTag_sl1 || o_slSetConstants_interposer_sl1 ||
        o_slEvaluateFeature_sl1)
        unhookInterposer();

    {
        char dllPath[MAX_PATH];
        GetModuleFileNameA(slInterposer, dllPath, MAX_PATH);

        LOG_TRACE("slInterposer path: {}", dllPath);

        version_t sl_version;
        Util::GetFileVersion(string_to_wstring(dllPath), &sl_version);

        State::Instance().streamlineVersion.major = sl_version.major;
        State::Instance().streamlineVersion.minor = sl_version.minor;
        State::Instance().streamlineVersion.patch = sl_version.patch;

        LOG_INFO("Streamline version: {}.{}.{}", sl_version.major, sl_version.minor, sl_version.patch);

        if (sl_version.major >= 2)
        {
            o_slSetTag =
                reinterpret_cast<decltype(&slSetTag)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTag"));
            o_slSetTagForFrame = reinterpret_cast<decltype(&slSetTagForFrame)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTagForFrame"));
            o_slInit = reinterpret_cast<decltype(&slInit)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slInit"));
            o_slEvaluateFeature = reinterpret_cast<decltype(&slEvaluateFeature)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slEvaluateFeature"));
            o_slAllocateResources = reinterpret_cast<decltype(&slAllocateResources)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slAllocateResources"));
            o_slSetConstants = reinterpret_cast<decltype(&slSetConstants)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetConstants"));
            o_slGetNativeInterface = reinterpret_cast<decltype(&slGetNativeInterface)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetNativeInterface"));
            o_slSetD3DDevice = reinterpret_cast<decltype(&slSetD3DDevice)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetD3DDevice"));
            o_slGetNewFrameToken = reinterpret_cast<decltype(&slGetNewFrameToken)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetNewFrameToken")); // Not hooked

            // For making the game think DLSSG is loaded and supported
            // but making SL not actually load the plugin
            o_slIsFeatureSupported = reinterpret_cast<decltype(&slIsFeatureSupported)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slIsFeatureSupported"));
            o_slIsFeatureLoaded = reinterpret_cast<decltype(&slIsFeatureLoaded)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slIsFeatureLoaded"));
            o_slGetFeatureRequirements = reinterpret_cast<decltype(&slGetFeatureRequirements)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetFeatureRequirements"));
            o_slGetFeatureVersion = reinterpret_cast<decltype(&slGetFeatureVersion)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetFeatureVersion"));
            o_slGetFeatureFunction = reinterpret_cast<decltype(&slGetFeatureFunction)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetFeatureFunction"));
            o_slSetFeatureLoaded = reinterpret_cast<decltype(&slSetFeatureLoaded)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetFeatureLoaded"));

            if (o_slInit != nullptr)
            {
                LOG_TRACE("Hooking v2");
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());

                DetourAttach(&(PVOID&) o_slInit, hkslInit);

                if (o_slEvaluateFeature != nullptr)
                    DetourAttach(&(PVOID&) o_slEvaluateFeature, hkslEvaluateFeature);

                if (State::Instance().activeFgInput == FGInput::NvngxFG ||
                    State::Instance().activeFgInput == FGInput::DLSSG)
                {
                    if (o_slSetTag != nullptr)
                        DetourAttach(&(PVOID&) o_slSetTag, hkslSetTag);

                    if (o_slSetTagForFrame != nullptr)
                        DetourAttach(&(PVOID&) o_slSetTagForFrame, hkslSetTagForFrame);

                    if (o_slSetConstants != nullptr)
                        DetourAttach(&(PVOID&) o_slSetConstants, hkslSetConstants);
                }

                if (State::Instance().activeFgInput == FGInput::DLSSG)
                {
                    if (o_slIsFeatureSupported != nullptr)
                        DetourAttach(&(PVOID&) o_slIsFeatureSupported, hkslIsFeatureSupported);

                    if (o_slIsFeatureLoaded != nullptr)
                        DetourAttach(&(PVOID&) o_slIsFeatureLoaded, hkslIsFeatureLoaded);

                    if (o_slGetFeatureRequirements != nullptr)
                        DetourAttach(&(PVOID&) o_slGetFeatureRequirements, hkslGetFeatureRequirements);

                    if (o_slGetFeatureVersion != nullptr)
                        DetourAttach(&(PVOID&) o_slGetFeatureVersion, hkslGetFeatureVersion);

                    if (o_slGetFeatureFunction != nullptr)
                        DetourAttach(&(PVOID&) o_slGetFeatureFunction, hkslGetFeatureFunction);

                    if (o_slSetFeatureLoaded != nullptr)
                        DetourAttach(&(PVOID&) o_slSetFeatureLoaded, hkslSetFeatureLoaded);
                }

                // if (o_slAllocateResources != nullptr)
                //     DetourAttach(&(PVOID&) o_slAllocateResources, hkslAllocateResources);

                // if (o_slGetNativeInterface != nullptr)
                //     DetourAttach(&(PVOID&) o_slGetNativeInterface, hkslGetNativeInterface);

                // if (o_slSetD3DDevice != nullptr)
                //     DetourAttach(&(PVOID&) o_slSetD3DDevice, hkslSetD3DDevice);

                auto detourResult = DetourTransactionCommit();
                if (detourResult != NO_ERROR)
                {
                    LOG_ERROR("Failed to hook sl.interposer v2: {:X}", detourResult);
                    o_slSetTag = nullptr;
                    o_slSetTagForFrame = nullptr;
                    o_slInit = nullptr;
                    o_slEvaluateFeature = nullptr;
                    o_slAllocateResources = nullptr;
                    o_slSetConstants = nullptr;
                    o_slGetNativeInterface = nullptr;
                    o_slSetD3DDevice = nullptr;
                    o_slIsFeatureSupported = nullptr;
                    o_slIsFeatureLoaded = nullptr;
                    o_slGetFeatureRequirements = nullptr;
                    o_slGetFeatureVersion = nullptr;
                    o_slGetFeatureFunction = nullptr;
                    o_slSetFeatureLoaded = nullptr;
                }
            }
        }
        else if (sl_version.major == 1)
        {
            o_slInit_sl1 =
                reinterpret_cast<decltype(&sl1::slInit)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slInit"));
            o_slSetTag_sl1 = reinterpret_cast<decltype(&sl1::slSetTag)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTag"));
            o_slSetConstants_interposer_sl1 = reinterpret_cast<decltype(&sl1::slSetConstants)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetConstants"));
            o_slEvaluateFeature_sl1 = reinterpret_cast<decltype(&sl1::slEvaluateFeature)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slEvaluateFeature"));

            LOG_INFO("SL1 exports - slInit: {}, slSetTag: {}, slSetConstants: {}, slEvaluateFeature: {}",
                     o_slInit_sl1 != nullptr, o_slSetTag_sl1 != nullptr, o_slSetConstants_interposer_sl1 != nullptr,
                     o_slEvaluateFeature_sl1 != nullptr);

            if (o_slInit_sl1 || o_slSetTag_sl1 || o_slSetConstants_interposer_sl1 || o_slEvaluateFeature_sl1)
            {
                LOG_TRACE("Hooking v1");
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());

                if (o_slInit_sl1)
                    DetourAttach(&(PVOID&) o_slInit_sl1, hkslInit_sl1);

                if (IsSL1AndFGActive())
                {
                    if (o_slSetTag_sl1)
                        DetourAttach(&(PVOID&) o_slSetTag_sl1, hkslSetTag_sl1);

                    if (o_slSetConstants_interposer_sl1)
                        DetourAttach(&(PVOID&) o_slSetConstants_interposer_sl1, hkslSetConstants_sl1);

                    if (o_slEvaluateFeature_sl1)
                        DetourAttach(&(PVOID&) o_slEvaluateFeature_sl1, hkslEvaluateFeature_sl1);
                }

                auto detourResult = DetourTransactionCommit();
                if (detourResult != NO_ERROR)
                {
                    LOG_ERROR("Failed to hook sl.interposer v1: {:X}", detourResult);
                    o_slInit_sl1 = nullptr;
                    o_slSetTag_sl1 = nullptr;
                    o_slSetConstants_interposer_sl1 = nullptr;
                    o_slEvaluateFeature_sl1 = nullptr;
                }
            }
        }
    }

    State::EnableChecks(owner);
}

// DLSS/DLSSG targeted port from upstream 4abfd7b, with per-instance callbacks.
StreamlineHooks::PluginCalls StreamlineHooks::pluginCalls[kPluginSlots];

void StreamlineHooks::hookPlugin(unsigned kind, HMODULE module)
{
    if (State::Instance().externalFrameGeneration || !module) return;
    // Exact loader notifications distinguish a reference decrement from real unload,
    // even when the next DLL is mapped at the very same address. The callback only
    // marks atomic tombstones; it never calls the loader or takes an application lock.
    if (!pluginModules.StartNotifications()) return;
    const auto target = reinterpret_cast<PFN_slGetPluginFunction>(
        KernelBaseProxy::GetProcAddress_()(module, "slGetPluginFunction"));
    const auto index = pluginModules.Hook(kind, module, target, [kind](size_t i, auto& original) {
        const auto hook = kind == 0 ? Callback<&hkdlss_slGetPluginFunction>(i)
                                    : Callback<&hkdlssg_slGetPluginFunction>(i);
        if (DetourTransactionBegin() != NO_ERROR) return false;
        if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
            DetourAttach(reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(hook)) != NO_ERROR)
        { DetourTransactionAbort(); return false; }
        const bool ok = DetourTransactionCommit() == NO_ERROR;
        if (ok)
        {
            if (kind == 0) pluginCalls[i].o_dlss_slGetPluginFunction.store(original);
            else pluginCalls[i].o_dlssg_slGetPluginFunction.store(original);
        }
        return ok;
    });
    if (index < 0) LOG_WARN("Streamline plugin hook unavailable; keeping existing instances and original new module");
}
void StreamlineHooks::hookDlss(HMODULE module) { hookPlugin(0, module); }
void StreamlineHooks::hookDlssg(HMODULE module) { hookPlugin(1, module); }

// Reflex / PCL / common / local DLSSG (D6): one hook per domain over bounded module slots. Every
// detour stays attached for the process lifetime (the interposer caches the addresses we hand out,
// a dead trampoline cannot be detached), the per-slot thunks keep a stable callback identity and
// pin their module while it runs.

template <size_t I> void* StreamlineHooks::localDlssgThunk(const char* functionName)
{
    if (!localDlssgModules.slots[I].live.load(std::memory_order_acquire))
        return nullptr;

    PluginModuleSlots<2>::Lease lease(localDlssgModules.slots[I]);

    if (!lease)
        return nullptr;

    currentLocalDlssgSlot = I;
    struct Restore
    {
        ~Restore() { currentLocalDlssgSlot = 2; }
    } restore;

    return hklocal_dlssg_slGetPluginFunction(functionName);
}

template <size_t I> void* StreamlineHooks::reflexThunk(const char* functionName)
{
    if (!reflexModules.slots[I].live.load(std::memory_order_acquire))
        return nullptr;

    PluginModuleSlots<2>::Lease lease(reflexModules.slots[I]);

    if (!lease)
        return nullptr;

    currentReflexSlot = I;
    struct Restore
    {
        ~Restore() { currentReflexSlot = 2; }
    } restore;

    return hkreflex_slGetPluginFunction(functionName);
}

template <size_t I> void* StreamlineHooks::pclThunk(const char* functionName)
{
    if (!pclModules.slots[I].live.load(std::memory_order_acquire))
        return nullptr;

    PluginModuleSlots<2>::Lease lease(pclModules.slots[I]);

    if (!lease)
        return nullptr;

    currentPclSlot = I;
    struct Restore
    {
        ~Restore() { currentPclSlot = 2; }
    } restore;

    return hkpcl_slGetPluginFunction(functionName);
}

template <size_t I> void* StreamlineHooks::commonThunk(const char* functionName)
{
    if (!commonModules.slots[I].live.load(std::memory_order_acquire))
        return nullptr;

    PluginModuleSlots<2>::Lease lease(commonModules.slots[I]);

    if (!lease)
        return nullptr;

    currentCommonSlot = I;
    struct Restore
    {
        ~Restore() { currentCommonSlot = 2; }
    } restore;

    return hkcommon_slGetPluginFunction(functionName);
}

template void* StreamlineHooks::localDlssgThunk<0>(const char*);
template void* StreamlineHooks::localDlssgThunk<1>(const char*);
template void* StreamlineHooks::reflexThunk<0>(const char*);
template void* StreamlineHooks::reflexThunk<1>(const char*);
template void* StreamlineHooks::pclThunk<0>(const char*);
template void* StreamlineHooks::pclThunk<1>(const char*);
template void* StreamlineHooks::commonThunk<0>(const char*);
template void* StreamlineHooks::commonThunk<1>(const char*);

namespace
{

// The slot registry shared by the four single-entry domains (D6)
template <size_t Capacity, typename HookOf, typename StoreOf>
void HookSingleModule(PluginModuleSlots<Capacity>& modules, HMODULE module, const char* logName, HookOf hookOf,
                      StoreOf storeOf)
{
    if (!module)
    {
        LOG_WARN("{} module is NULL", logName);
        return;
    }

    // Exact loader notifications distinguish a reference decrement from real unload,
    // even when the next DLL is mapped at the very same address (see hookPlugin).
    if (!modules.StartNotifications())
        return;

    const auto target = reinterpret_cast<PluginModuleSlots<Capacity>::Resolver>(
        KernelBaseProxy::GetProcAddress_()(module, "slGetPluginFunction"));

    const auto index = modules.Hook(0, module, target, [&](size_t i, auto& original) {
        if (DetourTransactionBegin() != NO_ERROR)
            return false;

        if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
            DetourAttach(reinterpret_cast<PVOID*>(&original), reinterpret_cast<PVOID>(hookOf(i))) != NO_ERROR)
        {
            DetourTransactionAbort();
            return false;
        }

        if (DetourTransactionCommit() != NO_ERROR)
            return false;

        // DetourAttach left the trampoline in original
        storeOf(i, original);
        return true;
    });

    if (index < 0)
        LOG_WARN("{} hook unavailable; keeping existing instances and original module", logName);
    else
        LOG_TRACE("Hooking slGetPluginFunction in {}", logName);
}
} // namespace

void StreamlineHooks::hookLocalDlssg(HMODULE slDlssg)
{
    if (State::Instance().externalFrameGeneration)
        return;

    static constexpr void* (*thunks[2])(const char*) = { &localDlssgThunk<0>, &localDlssgThunk<1> };

    HookSingleModule(localDlssgModules, slDlssg, "local sl.dlssg", [](size_t i) { return thunks[i]; },
                     [](size_t i, auto& original) {
                         localDlssgGetFn[i] = reinterpret_cast<PFN_slGetPluginFunction>(original);
                     });
}

void StreamlineHooks::hookReflex(HMODULE slReflex)
{
    if (State::Instance().externalFrameGeneration)
        return;

    static constexpr void* (*thunks[2])(const char*) = { &reflexThunk<0>, &reflexThunk<1> };

    HookSingleModule(reflexModules, slReflex, "sl.reflex", [](size_t i) { return thunks[i]; },
                     [](size_t i, auto& original) {
                         reflexGetFn[i] = reinterpret_cast<PFN_slGetPluginFunction>(original);
                     });
}

void StreamlineHooks::hookPcl(HMODULE slPcl)
{
    if (State::Instance().externalFrameGeneration)
        return;

    static constexpr void* (*thunks[2])(const char*) = { &pclThunk<0>, &pclThunk<1> };

    HookSingleModule(pclModules, slPcl, "sl.pcl", [](size_t i) { return thunks[i]; },
                     [](size_t i, auto& original) {
                         pclGetFn[i] = reinterpret_cast<PFN_slGetPluginFunction>(original);
                     });
}

void StreamlineHooks::hookCommon(HMODULE slCommon)
{
    if (State::Instance().externalFrameGeneration)
        return;

    static constexpr void* (*thunks[2])(const char*) = { &commonThunk<0>, &commonThunk<1> };

    HookSingleModule(commonModules, slCommon, "sl.common", [](size_t i) { return thunks[i]; },
                     [](size_t i, auto& original) {
                         commonGetFn[i] = reinterpret_cast<PFN_slGetPluginFunction>(original);
                     });
}

bool StreamlineHooks::isInterposerHooked() { return o_slInit != nullptr || o_slInit_sl1 != nullptr; }

bool StreamlineHooks::isDlssHooked() { return pluginModules.Any(0); }

bool StreamlineHooks::isDlssgHooked() { return pluginModules.Any(1); }

bool StreamlineHooks::isLocalDlssgHooked() { return localDlssgModules.Any(0); }

bool StreamlineHooks::isCommonHooked() { return commonModules.Any(0); }

bool StreamlineHooks::isPclHooked() { return pclModules.Any(0); }

bool StreamlineHooks::isReflexHooked() { return reflexModules.Any(0); }
