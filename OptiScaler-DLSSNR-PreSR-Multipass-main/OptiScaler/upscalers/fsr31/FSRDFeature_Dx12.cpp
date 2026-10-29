#include "pch.h"
#include <atomic>
#include <nvsdk_ngx_defs_dlssd.h>
#include <DirectXMath.h>
#include <d3d12sdklayers.h>
#include <cmath>
#include "NVNGX_Parameter.h"
#include <misc/SkipSpoof.h>
#include "hooks/Streamline_Hooks.h"
#include "resource_tracking/ResTrack_Dx12.h"
#include "FSRDFeature_Dx12.h"
#include "shaders/fsrd_preprocess/FSRDPreprocessor_Dx12.h"
#include "shaders/fsrd_preprocess/FSRDShaderUtils.h"
#include "MathUtils.h"
#include <dlssnr/DlssNr.h>
#include <dlssnr/submission/CommandListProxy.h>

using namespace DirectX;
using namespace OptiMath;

using FSRDConvDesc = FSRDPreprocessor_Dx12::ConversionDesc;
using FSRDCompDesc = FSRDPreprocessor_Dx12::CompositionDesc;

// RR 1.2 uses the existing FFX effect-id field. These assertions prevent a
// descriptor-id packing change from silently routing RR calls to another module.
static_assert(FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER == 0x00050001u);
static_assert(FFX_API_DISPATCH_DESC_TYPE_DENOISER == 0x00050041u);
static_assert(FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION == 0x00050043u);
static_assert(FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE == 0x00050044u);
static_assert(FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR == 0x00050045u);
static_assert(FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE == 0x00050047u);
static_assert(FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR == 0x00050048u);
static_assert(FFX_API_DISPATCH_DESC_TYPE_DENOISER_SPECULAR_OCCLUSION == 0x00050049u);
static_assert(sizeof(ffxCreateContextDescDenoiser) == 40u);
static_assert(sizeof(ffxDispatchDescDenoiser) == 448u);
static_assert(sizeof(ffxDispatchDescDenoiserAmbientOcclusion) == 120u);
static_assert(sizeof(ffxDispatchDescDenoiserDirectDiffuse) == 120u);
static_assert(sizeof(ffxDispatchDescDenoiserDirectSpecular) == 120u);
static_assert(sizeof(ffxDispatchDescDenoiserIndirectDiffuse) == 120u);
static_assert(sizeof(ffxDispatchDescDenoiserIndirectSpecular) == 120u);
static_assert(sizeof(ffxDispatchDescDenoiserSpecularOcclusion) == 120u);

static bool UseIndirectSignal(const CustomOptional<int>& setting)
{
    return std::clamp(setting.value_or_default(), 0, 1) == 1;
}

static ffxStructType_t GetDiffuseSignalDescType(const Config& cfg, ffxStructType_t automaticSignalType)
{
    if (!cfg.FfxDenoiserDiffuseSignalType.has_value())
        return automaticSignalType;

    return UseIndirectSignal(cfg.FfxDenoiserDiffuseSignalType) ? FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE
                                                               : FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE;
}

static ffxStructType_t GetSpecularSignalDescType(const Config& cfg, ffxStructType_t automaticSignalType)
{
    if (!cfg.FfxDenoiserSpecularSignalType.has_value())
        return automaticSignalType;

    return UseIndirectSignal(cfg.FfxDenoiserSpecularSignalType) ? FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR
                                                                : FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR;
}

static uint32_t GetSignalFlag(ffxStructType_t descriptorType)
{
    switch (descriptorType)
    {
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION:
        return FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION;
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE:
        return FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE;
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR:
        return FFX_DENOISER_SIGNAL_DIRECT_SPECULAR;
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE:
        return FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE;
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR:
        return FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR;
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_SPECULAR_OCCLUSION:
        return FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION;
    default:
        return FFX_DENOISER_SIGNAL_NONE;
    }
}

static const char* GetSignalTypeName(ffxStructType_t descriptorType)
{
    switch (descriptorType)
    {
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION:
        return "AmbientOcclusion";
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE:
        return "DirectDiffuse";
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR:
        return "DirectSpecular";
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE:
        return "IndirectDiffuse";
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR:
        return "IndirectSpecular";
    case FFX_API_DISPATCH_DESC_TYPE_DENOISER_SPECULAR_OCCLUSION:
        return "SpecularOcclusion";
    default:
        return "Unknown";
    }
}

class DenoiserOutputStateGuard
{
  public:
    DenoiserOutputStateGuard(const std::unique_ptr<FSRDPreprocessor_Dx12>& preprocessor,
                             ID3D12GraphicsCommandList* commandList)
        : _preprocessor(&preprocessor), _commandList(commandList)
    {
    }

    ~DenoiserOutputStateGuard()
    {
        // Context recreation may replace the preprocessor during an evaluation
        // (for example when Auto resolves a signal type). Follow the owning
        // unique_ptr instead of retaining a raw pointer to the destroyed object.
        if (_preprocessor && *_preprocessor)
            (*_preprocessor)->TransitionDenoiserOutputsToRead(_commandList);
    }

    DenoiserOutputStateGuard(const DenoiserOutputStateGuard&) = delete;
    DenoiserOutputStateGuard& operator=(const DenoiserOutputStateGuard&) = delete;

  private:
    const std::unique_ptr<FSRDPreprocessor_Dx12>* _preprocessor;
    ID3D12GraphicsCommandList* _commandList;
};

// Hands back any title-owned resource the conversion borrowed for the frame. The title
// finds its resource in the state it declared, whatever path this evaluation took.
class TitleInputStateGuard
{
  public:
    TitleInputStateGuard(const std::unique_ptr<FSRDPreprocessor_Dx12>& preprocessor,
                         ID3D12GraphicsCommandList* commandList)
        : _preprocessor(&preprocessor), _commandList(commandList)
    {
    }

    ~TitleInputStateGuard()
    {
        // Whether anything owes a transition back is the preprocessor's own
        // per-frame record, not a bool snapshotted before this frame's binding
        // decided it - the first frame would otherwise never restore.
        if (_preprocessor && *_preprocessor)
            (*_preprocessor)->RestoreTitleInputStates(_commandList);
    }

    TitleInputStateGuard(const TitleInputStateGuard&) = delete;
    TitleInputStateGuard& operator=(const TitleInputStateGuard&) = delete;

  private:
    const std::unique_ptr<FSRDPreprocessor_Dx12>* _preprocessor;
    ID3D12GraphicsCommandList* _commandList;
};

class EvaluationFrameGuard
{
  public:
    explicit EvaluationFrameGuard(long& frameCount) : _frameCount(frameCount) {}
    ~EvaluationFrameGuard() { ++_frameCount; }

    EvaluationFrameGuard(const EvaluationFrameGuard&) = delete;
    EvaluationFrameGuard& operator=(const EvaluationFrameGuard&) = delete;

  private:
    long& _frameCount;
};

/**
 * @brief Retrieves a column-major NGX matrix into the interop layer's row-major storage,
 * while retaining column-vector multiplication semantics.
 */
static bool TryGetNGXColumnVectorMatrix(const NVSDK_NGX_Parameter& ngxParams, const char* key,
                                        DirectX::XMMATRIX& outValue)
{
    float* pMat = nullptr;

    if (ngxParams.Get(key, (void**) &pMat) == NVSDK_NGX_Result_Success && pMat != nullptr)
    {
        XMMATRIX packedMatrix = {};
        memcpy_s(&packedMatrix, sizeof(packedMatrix), pMat, sizeof(float) * 16);
        outValue = XMMatrixTranspose(packedMatrix);
        return true;
    }

    return false;
}

template <typename T>
static bool TryGetLoggedResource(const NVSDK_NGX_Parameter& ngxParams, const char* key, T*& outValue)
{
    const bool success = TryGetNGXVoidPointer(ngxParams, key, outValue);

    if (success)
        LOG_DEBUG("{} exists..", key);
    else
        LOG_ERROR("{} is missing!!", key);

    return success;
}

static XMUINT2 GetSubrectBase(const NVSDK_NGX_Parameter& ngxParams, const char* xKey, const char* yKey)
{
    unsigned int x = 0;
    unsigned int y = 0;
    ngxParams.Get(xKey, &x);
    ngxParams.Get(yKey, &y);
    return { x, y };
}

static bool ValidateSourceExtent(const char* name, ID3D12Resource* resource, const XMUINT2& base, uint32_t width,
                                 uint32_t height)
{
    if (!resource)
        return false;

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const bool valid = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 &&
                       desc.DepthOrArraySize == 1 && uint64_t(base.x) + uint64_t(width) <= desc.Width &&
                       uint64_t(base.y) + uint64_t(height) <= desc.Height;
    if (!valid)
    {
        LOG_ERROR("[RR_INPUT] {} does not cover requested Texture2D subrect: resource={}x{}, base=({}, {}), "
                  "extent={}x{}, dimension={}, arrays={}, samples={}",
                  name, desc.Width, desc.Height, base.x, base.y, width, height, static_cast<uint32_t>(desc.Dimension),
                  desc.DepthOrArraySize, desc.SampleDesc.Count);
    }

    return valid;
}

enum class DepthResourceKind : uint8_t
{
    Unknown,       // Neither format nor flags say anything conclusive.
    HardwareDepth, // Depth-stencil capable, so it cannot hold a linear distance.
    GameWritten,   // Render-target or UAV capable: the title produced these contents.
};

/**
 * @brief Classifies the depth resource from its D3D12 description.
 *
 * A depth-stencil format or ALLOW_DEPTH_STENCIL is conclusive - no engine writes
 * linearized view-space distance into a depth-stencil attachment. Conversely a
 * render-target or UAV capable resource was produced by the title's own shaders,
 * which makes a linear declaration credible. Everything else, typically a plain
 * copy destination or a bare typeless resource, is genuinely undecidable here.
 */
static DepthResourceKind ClassifyDepthResource(ID3D12Resource* depth)
{
    if (!depth)
        return DepthResourceKind::Unknown;

    const D3D12_RESOURCE_DESC desc = depth->GetDesc();

    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0)
        return DepthResourceKind::HardwareDepth;

    switch (desc.Format)
    {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return DepthResourceKind::HardwareDepth;

    default:
        break;
    }

    if ((desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) != 0)
    {
        return DepthResourceKind::GameWritten;
    }

    return DepthResourceKind::Unknown;
}

// Whether a packed alpha is wide enough to be worth binding the denoiser's material guidance to.
//
// R10G10B10A2 has two bits of alpha -- four roughness levels -- and a title that does not pack
// roughness there leaves it at zero, which reads back as "every surface is a mirror" and costs the
// denoiser the guidance it needs most. A title that tags NormalRoughness through Streamline is
// saying where its roughness really lives, and that is worth preferring over this.
static bool AlphaCarriesRoughness(ID3D12Resource* normals);

static bool SupportsPackedRoughness(ID3D12Resource* normals)
{
    if (!normals)
        return false;

    switch (normals->GetDesc().Format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        return true;
    default:
        return false;
    }
}

static bool AlphaCarriesRoughness(ID3D12Resource* normals)
{
    if (!SupportsPackedRoughness(normals))
        return false;

    switch (normals->GetDesc().Format)
    {
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return false;
    default:
        return true;
    }
}

static void SetColumn(const XMVECTOR& vec, int col, XMMATRIX& mat)
{
    mat.r[0].m128_f32[col] = vec.m128_f32[0];
    mat.r[1].m128_f32[col] = vec.m128_f32[1];
    mat.r[2].m128_f32[col] = vec.m128_f32[2];
    mat.r[3].m128_f32[col] = vec.m128_f32[3];
}

static XMFLOAT3 GetFloat3Column(const XMMATRIX& mat, int col)
{
    return { mat.r[0].m128_f32[col], mat.r[1].m128_f32[col], mat.r[2].m128_f32[col] };
}

// A published matrix is only trustworthy if every element is finite. Titles that do
// not have a value to report sometimes publish a sentinel fill instead of omitting the
// parameter - an all-FLT_MAX matrix is one observed case - and such a matrix passes a
// presence check while making every derived quantity NaN.
static bool MatrixIsFinite(const XMMATRIX& matrix)
{
    for (int row = 0; row < 4; row++)
    {
        for (int column = 0; column < 4; column++)
        {
            if (!std::isfinite(matrix.r[row].m128_f32[column]))
                return false;
        }
    }

    return true;
}

/**
 * @brief Converts the interop layer's column-vector matrix into RR 1.2's canonical
 * row-major, row-vector layout.
 */
static FfxApiMatrix4x4 GetRRMatrix(const XMMATRIX& columnVectorMatrix)
{
    static_assert(sizeof(FfxApiMatrix4x4) == sizeof(XMFLOAT4X4));
    FfxApiMatrix4x4 result = {};
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(&result), XMMatrixTranspose(columnVectorMatrix));
    return result;
}

/**
 * @brief Uploads a column-vector matrix for HLSL's default column-major cbuffer
 * storage and mul(Matrix, Vector) usage.
 */
static void StoreHlslColumnVectorMatrix(XMFLOAT4X4& destination, const XMMATRIX& columnVectorMatrix)
{
    XMStoreFloat4x4(&destination, XMMatrixTranspose(columnVectorMatrix));
}

/**
 * @brief Creates an unjittered perspective projection in the interop layer's
 * column-vector convention. A zero far distance is treated as an infinite far plane.
 */
static XMMATRIX CreateColumnVectorPerspectiveProjection(float verticalFov, float aspectRatio, float nearPlane,
                                                        float farPlane, bool isRightHanded, bool isDepthInverted)
{
    XMMATRIX rowVectorProjection = {};

    if (farPlane == 0.0f)
    {
        const float yScale = 1.0f / std::tan(verticalFov * 0.5f);
        const float xScale = yScale / aspectRatio;
        const float W = isRightHanded ? -1.0f : 1.0f;
        const float A = isDepthInverted ? 0.0f : W;
        const float B = isDepthInverted ? nearPlane : -nearPlane;

        rowVectorProjection =
            XMMatrixSet(xScale, 0.0f, 0.0f, 0.0f, 0.0f, yScale, 0.0f, 0.0f, 0.0f, 0.0f, A, W, 0.0f, 0.0f, B, 0.0f);
    }
    else
    {
        // Swapping the physical near/far arguments produces a reversed-Z projection.
        const float matrixNear = isDepthInverted ? farPlane : nearPlane;
        const float matrixFar = isDepthInverted ? nearPlane : farPlane;

        rowVectorProjection = isRightHanded ? XMMatrixPerspectiveFovRH(verticalFov, aspectRatio, matrixNear, matrixFar)
                                            : XMMatrixPerspectiveFovLH(verticalFov, aspectRatio, matrixNear, matrixFar);
    }

    return XMMatrixTranspose(rowVectorProjection);
}

static ID3D12Resource* GetD3D12ResFromFFX(const FfxApiResource& resource)
{
    return static_cast<ID3D12Resource*>(resource.resource);
}

struct RequiredRRResource
{
    const char* name;
    FfxApiResource resource;
    DXGI_FORMAT format;
};

using RequiredRRResources = std::vector<RequiredRRResource>;

static RequiredRRResources GetRequiredRRResources(const ffxDispatchDescDenoiser& dispatchDesc,
                                                  const ffxDispatchDescDenoiserDirectDiffuse& directDiffuse,
                                                  const ffxDispatchDescDenoiserIndirectSpecular& indirectSpecular,
                                                  const ffxDispatchDescDenoiserAmbientOcclusion* ambientOcclusion)
{
    RequiredRRResources resources {
        { "LinearDepth", dispatchDesc.linearDepth, DXGI_FORMAT_R32_FLOAT },
        { "MotionVectors", dispatchDesc.motionVectors, DXGI_FORMAT_R16G16B16A16_FLOAT },
        { "Normals", dispatchDesc.normals, DXGI_FORMAT_R10G10B10A2_UNORM },
        { "SpecularAlbedo", dispatchDesc.specularAlbedo, DXGI_FORMAT_R8G8B8A8_UNORM },
        { "DiffuseAlbedo", dispatchDesc.diffuseAlbedo, DXGI_FORMAT_R8G8B8A8_UNORM },
        { "DiffuseSignal.Input", directDiffuse.signal.input, DXGI_FORMAT_R16G16B16A16_FLOAT },
        { "DiffuseSignal.Output", directDiffuse.signal.output, DXGI_FORMAT_R16G16B16A16_FLOAT },
        { "SpecularSignal.Input", indirectSpecular.signal.input, DXGI_FORMAT_R16G16B16A16_FLOAT },
        { "SpecularSignal.Output", indirectSpecular.signal.output, DXGI_FORMAT_R16G16B16A16_FLOAT },
    };

    if (ambientOcclusion)
    {
        resources.push_back({ "AmbientOcclusion.Input", ambientOcclusion->signal.input, DXGI_FORMAT_R8_UNORM });
        resources.push_back({ "AmbientOcclusion.Output", ambientOcclusion->signal.output, DXGI_FORMAT_R8_UNORM });
    }

    return resources;
}

static bool ValidateRequiredRRResources(const ffxDispatchDescDenoiser& dispatchDesc,
                                        const ffxDispatchDescDenoiserDirectDiffuse& directDiffuse,
                                        const ffxDispatchDescDenoiserIndirectSpecular& indirectSpecular,
                                        const ffxDispatchDescDenoiserAmbientOcclusion* ambientOcclusion)
{
    const RequiredRRResources requirements =
        GetRequiredRRResources(dispatchDesc, directDiffuse, indirectSpecular, ambientOcclusion);

    bool valid = true;

    for (const auto& requirement : requirements)
    {
        ID3D12Resource* resource = GetD3D12ResFromFFX(requirement.resource);

        if (!resource)
        {
            LOG_ERROR("Required RR 1.2 resource {} is null", requirement.name);
            valid = false;
            continue;
        }

        const D3D12_RESOURCE_DESC desc = resource->GetDesc();

        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
            desc.DepthOrArraySize != 1 || desc.Width < dispatchDesc.renderSize.width ||
            desc.Height < dispatchDesc.renderSize.height)
        {
            LOG_ERROR("Required RR 1.2 resource {} has unsupported layout or insufficient coverage: dimension={}, "
                      "arraySize={}, samples={}, size={}x{}; required coverage={}x{}",
                      requirement.name, magic_enum::enum_name(desc.Dimension), desc.DepthOrArraySize,
                      desc.SampleDesc.Count, desc.Width, desc.Height, dispatchDesc.renderSize.width,
                      dispatchDesc.renderSize.height);
            valid = false;
        }

        const DXGI_FORMAT viewFormat = FSRD::GetViewFormat(desc.Format);

        if (viewFormat != requirement.format)
        {
            LOG_ERROR("Required RR 1.2 resource {} has incompatible format {}; expected {}", requirement.name,
                      magic_enum::enum_name(viewFormat), magic_enum::enum_name(requirement.format));
            valid = false;
        }
    }

    return valid;
}

static void LogRRDispatchSnapshot(const ffxDispatchDescDenoiser& dispatchDesc,
                                  const ffxDispatchDescDenoiserDirectDiffuse& directDiffuse,
                                  const ffxDispatchDescDenoiserIndirectSpecular& indirectSpecular,
                                  const ffxDispatchDescDenoiserAmbientOcclusion* ambientOcclusion)
{
    ID3D12GraphicsCommandList* commandList = static_cast<ID3D12GraphicsCommandList*>(dispatchDesc.commandList);
    const D3D12_COMMAND_LIST_TYPE commandListType = commandList ? commandList->GetType() : D3D12_COMMAND_LIST_TYPE(-1);

    LOG_INFO("[RR_DIAG] dispatch snapshot: frame={}, reset={}, render={}x{}, commandList={:X}, commandListType={}, "
             "depthBounds=[{:.6f}, {:.6f}], mvScale=[{:.6f}, {:.6f}, {:.6f}], jitterPixels=[{:.6f}, {:.6f}], "
             "cameraDelta=[{:.6f}, {:.6f}, {:.6f}], flags={:#x}",
             dispatchDesc.frameIndex, !!(dispatchDesc.flags & FFX_DENOISER_DISPATCH_RESET),
             dispatchDesc.renderSize.width, dispatchDesc.renderSize.height,
             reinterpret_cast<uintptr_t>(dispatchDesc.commandList), magic_enum::enum_name(commandListType),
             dispatchDesc.linearDepthBounds.min, dispatchDesc.linearDepthBounds.max, dispatchDesc.motionVectorScale.x,
             dispatchDesc.motionVectorScale.y, dispatchDesc.motionVectorScale.z, dispatchDesc.jitterOffsets.x,
             dispatchDesc.jitterOffsets.y, dispatchDesc.cameraPositionDelta.x, dispatchDesc.cameraPositionDelta.y,
             dispatchDesc.cameraPositionDelta.z, dispatchDesc.flags);

    std::string chain = std::format("head={:#x}", dispatchDesc.header.type);
    for (const ffxDispatchDescHeader* signal = dispatchDesc.header.pNext; signal != nullptr; signal = signal->pNext)
    {
        chain += std::format(" -> {}={:#x}", GetSignalTypeName(signal->type), signal->type);
    }
    LOG_INFO("[RR_DIAG] chain: {} -> tail=0x0", chain);

    const RequiredRRResources requirements =
        GetRequiredRRResources(dispatchDesc, directDiffuse, indirectSpecular, ambientOcclusion);

    for (const RequiredRRResource& requirement : requirements)
    {
        ID3D12Resource* resource = GetD3D12ResFromFFX(requirement.resource);
        if (!resource)
        {
            LOG_ERROR("[RR_DIAG] resource {}: null", requirement.name);
            continue;
        }

        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        LOG_INFO("[RR_DIAG] resource {}: ptr={:X}, size={}x{}, format={}, dimension={}, mips={}, samples={}, "
                 "resourceFlags={:#x}, declaredFfxState={:#x}",
                 requirement.name, reinterpret_cast<uintptr_t>(resource), desc.Width, desc.Height,
                 magic_enum::enum_name(FSRD::GetViewFormat(desc.Format)), magic_enum::enum_name(desc.Dimension),
                 desc.MipLevels, desc.SampleDesc.Count, static_cast<uint32_t>(desc.Flags), requirement.resource.state);
    }
}

static bool IsDiffuseHitDistanceFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R16_FLOAT || format == DXGI_FORMAT_R32_FLOAT;
}

// A responsivity hint is a per-pixel scalar. Titles have been seen to publish it under
// their own NGX key, so the format is accepted broadly and only its coverage is required.
static bool IsResponsivityMaskFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

static bool IsDiffuseRayDirectionHitDistanceFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

static bool CoversSourceExtent(ID3D12Resource* resource, const XMUINT2& base, uint32_t width, uint32_t height)
{
    if (!resource)
        return false;

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 &&
           uint64_t(base.x) + uint64_t(width) <= desc.Width && uint64_t(base.y) + uint64_t(height) <= desc.Height;
}

using SourceFormatValidator = bool (*)(DXGI_FORMAT);

static bool ValidateReprojectionGuideSource(const char* name, ID3D12Resource* resource, const XMUINT2& base,
                                            uint32_t width, uint32_t height, SourceFormatValidator validateFormat,
                                            const char* expectedFormat)
{
    if (!resource)
        return false;

    const DXGI_FORMAT viewFormat = FSRD::GetViewFormat(resource->GetDesc().Format);
    if (!validateFormat(viewFormat))
    {
        LOG_ERROR("[RR_INPUT] {} has incompatible format {}; expected {}", name, magic_enum::enum_name(viewFormat),
                  expectedFormat);
        return false;
    }

    return ValidateSourceExtent(name, resource, base, width, height);
}

static bool IsEmissiveProbeCompatible(ID3D12Resource* resource)
{
    if (!resource)
        return false;

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0 ||
        desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1)
    {
        return false;
    }

    const DXGI_FORMAT viewFormat = FSRD::GetViewFormat(desc.Format);
    switch (viewFormat)
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
}

static void LogRREmissiveProbe(ID3D12Resource* resource, bool compatible, bool fromStreamline, uint32_t renderWidth,
                               uint32_t renderHeight)
{
    if (!resource)
    {
        LOG_INFO("[RR_DIAG] emissive probe: absent (checked NGX {} and Streamline Emissive tag)",
                 NVSDK_NGX_Parameter_GBuffer_Emissive);
        return;
    }

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const DXGI_FORMAT viewFormat = FSRD::GetViewFormat(desc.Format);
    LOG_INFO("[RR_DIAG] emissive probe: present, source={}, ptr={:X}, size={}x{}, format={}, dimension={}, mips={}, "
             "samples={}, "
             "resourceFlags={:#x}, renderSize={}x{}, exactRenderSize={}, previewCompatible={}",
             fromStreamline ? "Streamline.Emissive" : NVSDK_NGX_Parameter_GBuffer_Emissive,
             reinterpret_cast<uintptr_t>(resource), desc.Width, desc.Height, magic_enum::enum_name(viewFormat),
             magic_enum::enum_name(desc.Dimension), desc.MipLevels, desc.SampleDesc.Count,
             static_cast<uint32_t>(desc.Flags), renderWidth, renderHeight,
             desc.Width == renderWidth && desc.Height == renderHeight, compatible);
}

static void LogRRDiffuseHitDistanceProbe(const char* parameterName, ID3D12Resource* resource, uint32_t subrectBaseX,
                                         uint32_t subrectBaseY, uint32_t renderWidth, uint32_t renderHeight,
                                         bool combinedDirectionAndDistance)
{
    if (!resource)
    {
        LOG_INFO("[RR_DIAG] NGX probe {}: absent", parameterName);
        return;
    }

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const DXGI_FORMAT viewFormat = FSRD::GetViewFormat(desc.Format);
    const bool compatibleFormat = combinedDirectionAndDistance ? IsDiffuseRayDirectionHitDistanceFormat(viewFormat)
                                                               : IsDiffuseHitDistanceFormat(viewFormat);
    const uint64_t requiredWidth = static_cast<uint64_t>(subrectBaseX) + renderWidth;
    const uint64_t requiredHeight = static_cast<uint64_t>(subrectBaseY) + renderHeight;
    const bool coversRenderArea = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width >= requiredWidth &&
                                  desc.Height >= requiredHeight;

    LOG_INFO("[RR_DIAG] NGX probe {}: present, ptr={:X}, size={}x{}, format={}, dimension={}, mips={}, samples={}, "
             "resourceFlags={:#x}, subrectBase=[{}, {}], expected={}, compatibleFormat={}, coversRenderArea={}",
             parameterName, reinterpret_cast<uintptr_t>(resource), desc.Width, desc.Height,
             magic_enum::enum_name(viewFormat), magic_enum::enum_name(desc.Dimension), desc.MipLevels,
             desc.SampleDesc.Count, static_cast<uint32_t>(desc.Flags), subrectBaseX, subrectBaseY,
             combinedDirectionAndDistance ? "RGBA16_FLOAT/RGBA32_FLOAT (distance in A)" : "R16_FLOAT/R32_FLOAT",
             compatibleFormat, coversRenderArea);
}

static void LogRRGBufferIdentityProbe(const char* parameterName, ID3D12Resource* resource, uint32_t renderWidth,
                                      uint32_t renderHeight)
{
    if (!resource)
    {
        LOG_INFO("[RR_DIAG] NGX identity probe {}: absent", parameterName);
        return;
    }

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const DXGI_FORMAT viewFormat = FSRD::GetViewFormat(desc.Format);
    const bool exactRenderSize = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == renderWidth &&
                                 desc.Height == renderHeight;

    LOG_INFO(
        "[RR_DIAG] NGX identity probe {}: present, ptr={:X}, size={}x{}, format={}, dimension={}, mips={}, samples={}, "
        "resourceFlags={:#x}, exactRenderSize={}; diagnostic-only, value encoding and temporal stability are "
        "unverified",
        parameterName, reinterpret_cast<uintptr_t>(resource), desc.Width, desc.Height,
        magic_enum::enum_name(viewFormat), magic_enum::enum_name(desc.Dimension), desc.MipLevels, desc.SampleDesc.Count,
        static_cast<uint32_t>(desc.Flags), exactRenderSize);
}

static void LogRRD3D12Messages(ID3D12InfoQueue* infoQueue, uint64_t firstMessage, std::string_view scope)
{
    if (!infoQueue)
        return;

    const uint64_t messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    firstMessage = std::min(firstMessage, messageCount);

    constexpr uint64_t kMaxLoggedMessages = 32;
    uint64_t loggedMessages = 0;
    uint64_t matchingMessages = 0;

    for (uint64_t messageIndex = firstMessage; messageIndex < messageCount; ++messageIndex)
    {
        SIZE_T messageSize = 0;
        if (FAILED(infoQueue->GetMessage(messageIndex, nullptr, &messageSize)) || messageSize == 0)
            continue;

        std::vector<uint8_t> storage(messageSize);
        D3D12_MESSAGE* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (FAILED(infoQueue->GetMessage(messageIndex, message, &messageSize)))
            continue;

        if (message->Severity > D3D12_MESSAGE_SEVERITY_WARNING)
            continue;

        ++matchingMessages;
        if (loggedMessages >= kMaxLoggedMessages)
            continue;

        ++loggedMessages;
        LOG_WARN("[RR_DIAG][D3D12][{}] severity={}, category={}, id={} ({}) - {}", scope,
                 magic_enum::enum_name(message->Severity), magic_enum::enum_name(message->Category),
                 static_cast<uint32_t>(message->ID), magic_enum::enum_name(message->ID), message->pDescription);
    }

    if (matchingMessages == 0)
    {
        LOG_INFO("[RR_DIAG][D3D12][{}] no corruption/error/warning messages were captured", scope);
    }
    else if (matchingMessages > loggedMessages)
    {
        LOG_WARN("[RR_DIAG][D3D12][{}] {} additional messages were omitted", scope, matchingMessages - loggedMessages);
    }
}

struct ViewPlanes
{
    float nearPlane;
    float farPlane;
    bool isInfinite;
    bool isRightHanded;
};

static ViewPlanes GetViewPlanes(const DirectX::XMMATRIX& projection, bool isInverted)
{
    ViewPlanes planes = {};

    // Internal projection convention: row-major storage with column vectors.
    // clip.z = A * view.z + B; clip.w = W * view.z.
    const float A = projection.r[2].m128_f32[2];
    const float B = projection.r[2].m128_f32[3];
    const float W = projection.r[3].m128_f32[2];

    // A is the projection's depth term and W its perspective divide. On an infinite
    // far plane A is mathematically zero, but it survives matrix inversion as float
    // noise - values around 1e-6 are routine - so an absolute epsilon misclassifies
    // the projection as finite and the far plane below is then computed by dividing
    // by that noise. Cyberpunk lands on A = -1.19e-6 and produces a fabricated
    // 16777 unit far plane that way. Scale the test to W so it measures a ratio.
    const float infiniteCheckVal = isInverted ? A : (A - W);
    const float infiniteCheckScale = std::max(std::abs(W), 1e-12f);
    planes.isInfinite = std::abs(infiniteCheckVal) < 1e-4f * infiniteCheckScale;
    // W is +1 for LH projections and -1 for RH projections, independent of Z direction.
    planes.isRightHanded = W < 0.0f;

    // An infinite projection has no true far plane, but this value is consumed as a
    // log-normalisation denominator, as a depth clamp, and as RR's linearDepthBounds.
    // FLT_MAX degrades all three - log(3.4e38) is 88.7, which flattens every
    // realistic depth to the bottom tenth of the range - so report a large finite
    // horizon instead. FP16 max keeps it representable everywhere downstream.
    constexpr float kInfiniteHorizon = 65504.0f;

    if (isInverted)
    {
        // Inverted: Near is at D=1, Far is at D=0
        // 1 = A/W + B/(n*W) -> n = B / (W - A)
        planes.nearPlane = std::abs(B / (W - A));

        // 0 = A/W + B/(f*W) -> f = -B / A
        planes.farPlane = planes.isInfinite ? kInfiniteHorizon : std::abs(-B / A);
    }
    else
    {
        // Standard: Near is at D=0, Far is at D=1
        // 0 = A/W + B/(n*W) -> n = -B / A
        planes.nearPlane = std::abs(-B / A);

        // 1 = A/W + B/(f*W) -> f = B / (W - A)
        planes.farPlane = planes.isInfinite ? kInfiniteHorizon : std::abs(B / (W - A));
    }

    // A finite branch that survived the ratio test can still be wildly off if the
    // projection terms are noisy, so keep the result inside a range that cannot
    // break the consumers above.
    if (!std::isfinite(planes.nearPlane) || planes.nearPlane <= 0.0f)
    {
        // A degenerate or noisy projection divides by a term that is effectively
        // zero above. The near plane is consumed directly as RR's
        // linearDepthBounds.min and as the conversion shaders' depth clamp, so a
        // NaN here spreads into every reconstructed view-space position. Substitute
        // a usable default; the caller logs the resulting planes when they change.
        constexpr float kDefaultNearPlane = 0.01f;
        planes.nearPlane = kDefaultNearPlane;
        planes.farPlane = kInfiniteHorizon;
    }
    else if (!std::isfinite(planes.farPlane))
    {
        // std::clamp propagates NaN rather than replacing it, so filter it first.
        planes.farPlane = kInfiniteHorizon;
    }
    else
    {
        planes.farPlane = std::clamp(planes.farPlane, planes.nearPlane * 2.0f, kInfiniteHorizon);
    }

    return planes;
}

using FSRDConvFlags = FSRDPreprocessor_Dx12::ConvFlags;
using FSRDCompFlags = FSRDPreprocessor_Dx12::CompFlags;

enum class DebugModes : uint64_t
{
    None = 0,
    DenoiserBypass = 1,
    UpscalerBypass = 2,
    RawColor = 3,
    DlssBias = 4,
    DlssColorBeforeParticles = 5,
    DlssColorBeforeTransparency = 6,
    DlssTransparencyLayer = 7,
    FfxDebug = 8,
    AmbientOcclusionInput = 9,
    AmbientOcclusionOutput = 10,

    ConversionDebug = FSRDConvFlags::Debug,
    ConversionDebugMask = FSRDConvFlags::DebugModeMask,

    OutRadiance = FSRDConvFlags::DebugOutRadiance,

    InSpecHitDist = FSRDConvFlags::DebugInSpecHitDist,
    InMotion = FSRDConvFlags::DebugInMotion,
    InNormals = FSRDConvFlags::DebugInNormals,
    InRoughness = FSRDConvFlags::DebugInRoughness,
    InDiffAlbedo = FSRDConvFlags::DebugInDiffAlbedo,
    InSpecAlbedo = FSRDConvFlags::DebugInSpecAlbedo,

    OutSignalSplit = FSRDConvFlags::DebugOutSignalSplit,
    OutLinearDepth = FSRDConvFlags::DebugOutLinearDepth,
    OutMotion = FSRDConvFlags::DebugOutMotion,
    OutNormals = FSRDConvFlags::DebugOutNormals,
    OutSpecAlbedo = FSRDConvFlags::DebugOutSpecAlbedo,
    OutDiffAlbedo = FSRDConvFlags::DebugOutDiffAlbedo,

    OutDepthDelta = FSRDConvFlags::DebugOutDepthDelta,
    NormDepth = FSRDConvFlags::DebugNormDepth,
    AlbedoError = FSRDConvFlags::DebugAlbedoError,

    FloorColor = FSRDConvFlags::DebugFloorColor,
    RawIndirectSpecular = FSRDConvFlags::DebugRawIndirectSpecular,
    EffectiveRoughness = FSRDConvFlags::DebugEffectiveRoughness,
    RawRoughness = FSRDConvFlags::DebugRawRoughness,
    EmissiveMask = FSRDConvFlags::DebugEmissiveMask,
    AppliedRoughnessFloor = FSRDConvFlags::DebugAppliedRoughnessFloor,
    ResourceInspector = FSRDConvFlags::DebugResourceInspector,
    MaterialType = FSRDConvFlags::DebugMaterialType,
    InEmissive = FSRDConvFlags::DebugInEmissive,
    RRMaterialType = FSRDConvFlags::DebugRRMaterialType,
    AlbedoStructure = FSRDConvFlags::DebugAlbedoStructure,
    FloorHandoverOutput = FSRDConvFlags::DebugFloorHandover,

    SpecularSplit = FSRDConvFlags::DebugSpecularSplit,
    InTitleLinearDepth = FSRDConvFlags::DebugInTitleLinearDepth,
    TitleLinearDepthDiff = FSRDConvFlags::DebugTitleLinearDepthDiff,
    InResponsivityMask = FSRDConvFlags::DebugInResponsivityMask,

    InBiasMask = FSRDConvFlags::DebugInBiasMask,
    FloorStructure = FSRDConvFlags::DebugFloorStructure,
    SkipUnmapped = FSRDConvFlags::DebugSkipUnmapped,
    SkipFloor = FSRDConvFlags::DebugSkipFloor,
    SkipRawInject = FSRDConvFlags::DebugSkipRawInject,
    DemodGain = FSRDConvFlags::DebugDemodGain,
    HitDistGate = FSRDConvFlags::DebugHitDistGate,
    DenoiserFraction = FSRDConvFlags::DebugDenoiserFraction,

    CompositionDebugOffset = 16u,
    CompositionDebug = (uint64_t) FSRDCompFlags::Debug << CompositionDebugOffset,
    CompositionDebugMask = (uint64_t) FSRDCompFlags::DebugModeMask,

    Correlation = (uint64_t) FSRDCompFlags::DebugCorrelation << CompositionDebugOffset,
    SkipSignal = (uint64_t) FSRDCompFlags::DebugSkipSignal << CompositionDebugOffset,
    DenoiserOutput = (uint64_t) FSRDCompFlags::DebugDenoiserOutput << CompositionDebugOffset,
    DirectSpecular = (uint64_t) FSRDCompFlags::DebugDirectSpecular << CompositionDebugOffset,
    IndirectSpecular = (uint64_t) FSRDCompFlags::DebugIndirectSpecular << CompositionDebugOffset,
    DirectDiffuse = (uint64_t) FSRDCompFlags::DebugDirectDiffuse << CompositionDebugOffset,
    IndirectDiffuse = (uint64_t) FSRDCompFlags::DebugIndirectDiffuse << CompositionDebugOffset,
    HandoverRRBand = (uint64_t) FSRDCompFlags::DebugHandoverRRBand << CompositionDebugOffset,
    HandoverDetailBand = (uint64_t) FSRDCompFlags::DebugHandoverDetailBand << CompositionDebugOffset,
    HandoverBandMix = (uint64_t) FSRDCompFlags::DebugHandoverBandMix << CompositionDebugOffset,
    HandoverAnchor = (uint64_t) FSRDCompFlags::DebugHandoverAnchor << CompositionDebugOffset,
    HandoverWeight = (uint64_t) FSRDCompFlags::DebugHandoverWeight << CompositionDebugOffset,
};

static FSRDConvFlags GetConvDebugFlags(DebugModes mode)
{
    uint32_t flags = uint32_t(mode);
    flags &= uint32_t(DebugModes::ConversionDebugMask);
    return FSRDConvFlags(flags);
}

static FSRDCompFlags GetCompDebugFlags(DebugModes mode)
{
    uint64_t flags = uint64_t(mode);
    flags >>= uint64_t(DebugModes::CompositionDebugOffset);
    flags &= uint64_t(DebugModes::CompositionDebugMask);
    return FSRDCompFlags(flags);
}

using ModeNamePair = std::pair<const char*, uint64_t>;
constexpr auto kDebugModes = std::to_array<ModeNamePair>({
    { "None", (uint64_t) DebugModes::None },
    { "DebugOverview", (uint64_t) DebugModes::FfxDebug },

    { "DenoiserBypass", (uint64_t) DebugModes::DenoiserBypass },
    { "UpscalerBypass", (uint64_t) DebugModes::UpscalerBypass },
    { "DenoiserOutput", (uint64_t) DebugModes::DenoiserOutput },
    { "SkipSignal", (uint64_t) DebugModes::SkipSignal },

    { "RawColor", (uint64_t) DebugModes::RawColor },
    { "DlssBias", (uint64_t) DebugModes::DlssBias },
    { "DlssColorBeforeParticles", (uint64_t) DebugModes::DlssColorBeforeParticles },
    { "DlssColorBeforeTransparency", (uint64_t) DebugModes::DlssColorBeforeTransparency },
    { "DlssTransparencyLayer", (uint64_t) DebugModes::DlssTransparencyLayer },
    { "AmbientOcclusionInput", (uint64_t) DebugModes::AmbientOcclusionInput },
    { "AmbientOcclusionOutput", (uint64_t) DebugModes::AmbientOcclusionOutput },

    { "InputMotionVectors", (uint64_t) DebugModes::InMotion },
    { "InNormals", (uint64_t) DebugModes::InNormals },
    { "InputRoughness", (uint64_t) DebugModes::InRoughness },
    { "RawRoughness", (uint64_t) DebugModes::RawRoughness },
    { "EmissiveMask", (uint64_t) DebugModes::EmissiveMask },
    { "AppliedRoughnessFloor", (uint64_t) DebugModes::AppliedRoughnessFloor },
    { "ZeroRoughnessMaterialType", (uint64_t) DebugModes::MaterialType },
    { "ResourceInspector", (uint64_t) DebugModes::ResourceInspector },
    { "SpecularHitDistance", (uint64_t) DebugModes::InSpecHitDist },
    { "InDiffAlbedo", (uint64_t) DebugModes::InDiffAlbedo },
    { "InSpecAlbedo", (uint64_t) DebugModes::InSpecAlbedo },
    { "InputEmissive", (uint64_t) DebugModes::InEmissive },
    { "RRMaterialType", (uint64_t) DebugModes::RRMaterialType },
    { "AlbedoStructureAvailability", (uint64_t) DebugModes::AlbedoStructure },
    { "FloorHandoverOutput", (uint64_t) DebugModes::FloorHandoverOutput },

    { "SpecularSplit", (uint64_t) DebugModes::SpecularSplit },
    { "InTitleLinearDepth", (uint64_t) DebugModes::InTitleLinearDepth },
    { "TitleLinearDepthDiff", (uint64_t) DebugModes::TitleLinearDepthDiff },
    { "InResponsivityMask", (uint64_t) DebugModes::InResponsivityMask },

    { "InBiasMask", (uint64_t) DebugModes::InBiasMask },
    { "FloorStructure", (uint64_t) DebugModes::FloorStructure },
    { "SkipUnmapped", (uint64_t) DebugModes::SkipUnmapped },
    { "SkipFloor", (uint64_t) DebugModes::SkipFloor },
    { "SkipRawInject", (uint64_t) DebugModes::SkipRawInject },
    { "DemodGain", (uint64_t) DebugModes::DemodGain },
    { "HitDistGate", (uint64_t) DebugModes::HitDistGate },
    { "DenoiserFraction", (uint64_t) DebugModes::DenoiserFraction },

    { "OutRadiance", (uint64_t) DebugModes::OutRadiance },
    { "OutSignalSplit", (uint64_t) DebugModes::OutSignalSplit },
    { "OutLinearDepth", (uint64_t) DebugModes::OutLinearDepth },
    { "RRMotionVectors", (uint64_t) DebugModes::OutMotion },
    { "OutNormals", (uint64_t) DebugModes::OutNormals },
    { "OutSpecAlbedo", (uint64_t) DebugModes::OutSpecAlbedo },
    { "OutDiffAlbedo", (uint64_t) DebugModes::OutDiffAlbedo },
    { "OutDepthDelta", (uint64_t) DebugModes::OutDepthDelta },
    { "NormDepth", (uint64_t) DebugModes::NormDepth },

    { "AlbedoError", (uint64_t) DebugModes::AlbedoError },
    { "Correlation", (uint64_t) DebugModes::Correlation },

    { "HandoverRRLowBand", (uint64_t) DebugModes::HandoverRRBand },
    { "HandoverDetailHighBand", (uint64_t) DebugModes::HandoverDetailBand },
    { "HandoverBandMix", (uint64_t) DebugModes::HandoverBandMix },
    { "HandoverRRAnchor", (uint64_t) DebugModes::HandoverAnchor },
    { "HandoverEffectiveWeight", (uint64_t) DebugModes::HandoverWeight },

    { "FloorColor", (uint64_t) DebugModes::FloorColor },

    { "RawSpecularSignal", (uint64_t) DebugModes::RawIndirectSpecular },
    { "DenoisedDirectSpecSignal", (uint64_t) DebugModes::DirectSpecular },
    { "DenoisedIndirectSpecSignal", (uint64_t) DebugModes::IndirectSpecular },
    { "EffectiveRoughness", (uint64_t) DebugModes::EffectiveRoughness },
    { "DenoisedDirectDiffuseSignal", (uint64_t) DebugModes::DirectDiffuse },
    { "DenoisedIndirectDiffuseSignal", (uint64_t) DebugModes::IndirectDiffuse },
});

FSRDFeatureDx12::FSRDFeatureDx12(uint32_t InHandleId, NVSDK_NGX_Parameter* InParameters)
    : FSR31FeatureDx12(InHandleId, InParameters), IFeature(InHandleId, SetParameters(InParameters)),
      _pDenoiserCtx(nullptr), _denoiserCtxDesc({}), _denoiserSettings({}), _convDesc({})
{
    _moduleLoaded = FfxApiProxy::IsDenoiserApiImplementedDx12();

    if (FfxApiProxy::IsDenoiserApiImplementedDx12())
        LOG_INFO("amd_fidelityfx_denoiser_dx12.dll methods loaded!");
    else if (FfxApiProxy::IsDenoiserReady())
    {
        const feature_version version = FfxApiProxy::VersionDx12_RR();
        LOG_ERROR("amd_fidelityfx_denoiser_dx12.dll {}.{}.{} loaded, but this dispatch backend is not implemented",
                  version.major, version.minor, version.patch);
    }
    else
        LOG_ERROR("can't load amd_fidelityfx_denoiser_dx12.dll methods!");
}

FSRDFeatureDx12::~FSRDFeatureDx12()
{
    if (State::Instance().isShuttingDown)
        return;

    DestroyDenoiserContext();
}

bool FSRDFeatureDx12::RecordingsRetired()
{
    auto& tracker = DlssNr::Submission::GpuBatch::Tracker::Instance();
    std::erase_if(_recordingTickets, [&tracker](uint64_t ticket) { return tracker.Retired(ticket); });
    return _recordingTickets.empty();
}

namespace
{
std::mutex g_deferredFeaturesMutex;
std::vector<std::unique_ptr<FSRDFeatureDx12>> g_deferredFeatures;
}

void FSRDFeatureDx12::DestroyWhenRetired(std::unique_ptr<FSRDFeatureDx12> feature)
{
    if (!feature)
        return;
    if (feature->RecordingsRetired())
        return; // unique_ptr destroys the retired feature here

    LOG_INFO("FSR-RR feature deferred: {} recording(s) still outstanding at teardown",
             feature->_recordingTickets.size());
    std::lock_guard lock(g_deferredFeaturesMutex);
    g_deferredFeatures.push_back(std::move(feature));
}

void FSRDFeatureDx12::ReapDeferredFeatures()
{
    std::vector<std::unique_ptr<FSRDFeatureDx12>> ripe;
    {
        std::lock_guard lock(g_deferredFeaturesMutex);
        std::erase_if(g_deferredFeatures, [&ripe](std::unique_ptr<FSRDFeatureDx12>& feature) {
            if (feature->RecordingsRetired())
            {
                ripe.push_back(std::move(feature));
                return true;
            }
            return false;
        });
    }
    // Destruction happens outside the mutex: DestroyDenoiserContext talks to the proxy.
}

bool FSRDFeatureDx12::AcquireSLTaggedResource(const RRD3D12SignalTagSnapshot& snapshot, RRTaggedSignal signal,
                                              const char* sourceName, TagStatePolicy statePolicy,
                                              Microsoft::WRL::ComPtr<ID3D12Resource>& resource,
                                              RRTaggedResourceDiagnostic& diagnostic)
{
    resource.Reset();
    diagnostic = {};

    const size_t index = static_cast<size_t>(signal);
    if (index >= snapshot.resources.size())
        return false;

    const auto& entry = snapshot.resources[index];
    diagnostic = entry.diagnostic;
    if (!diagnostic.observed || !diagnostic.present)
        return false;

    if (diagnostic.lifecycle == sl::ResourceLifecycle::eOnlyValidNow &&
        diagnostic.source != RRTagSource::EvaluateFeature)
    {
        LOG_DEBUG("[RR_INPUT] {} uses Streamline eOnlyValidNow outside the active EvaluateFeature call; refusing a "
                  "cached binding",
                  sourceName);
        return false;
    }

    const uint32_t activeFrame = snapshot.activeEvaluationFrame;
    const uint32_t activeViewport = snapshot.activeEvaluationViewport;
    if (activeFrame == UINT32_MAX || activeViewport == UINT32_MAX)
    {
        LOG_DEBUG("[RR_INPUT] {} has no active Streamline frame/viewport; refusing an uncorrelated tag", sourceName);
        return false;
    }

    if (diagnostic.viewport != activeViewport)
    {
        LOG_DEBUG(
            "[RR_INPUT] {} belongs to Streamline viewport {}, active viewport is {}; rejecting cross-viewport tag",
            sourceName, diagnostic.viewport, activeViewport);
        return false;
    }

    const bool isLegacyTag = diagnostic.frameIndex == UINT32_MAX;

    if (!isLegacyTag)
    {
        if (diagnostic.frameIndex != activeFrame)
        {
            LOG_DEBUG("[RR_INPUT] {} belongs to Streamline frame {}, active frame is {}; rejecting stale tag",
                      sourceName, diagnostic.frameIndex, activeFrame);
            return false;
        }
    }
    else
    {
        // Legacy slSetTag has no frame token. Allow a newly submitted update and
        // repeated acquisition during this same evaluation, but never reuse it in
        // a later frame after its Present/Evaluate lifetime may have expired.
        const uint64_t lastUpdate = _lastConsumedSLTagUpdates[index];
        const uint32_t lastFrame = _lastConsumedSLTagFrames[index];
        if (diagnostic.updateCount < lastUpdate || (diagnostic.updateCount == lastUpdate && lastFrame != activeFrame))
        {
            LOG_DEBUG("[RR_INPUT] {} legacy tag update {} was already consumed in frame {}; rejecting stale reuse in "
                      "frame {}",
                      sourceName, diagnostic.updateCount, lastFrame, activeFrame);
            return false;
        }
    }

    if (!entry.resource || entry.resource.Get() != diagnostic.resourceAddress)
    {
        LOG_WARN("[RR_INPUT] {} atomic tag snapshot has no matching retained D3D12 resource (generation={})",
                 sourceName, snapshot.generation);
        return false;
    }

    if (diagnostic.state == UINT32_MAX)
    {
        LOG_WARN("[RR_INPUT] {} has no declared D3D12 resource state", sourceName);
        return false;
    }

    const D3D12_RESOURCE_STATES declaredState = static_cast<D3D12_RESOURCE_STATES>(diagnostic.state);
    const bool declaredShaderReadable = (declaredState & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
    const bool declaredStateAcceptable =
        statePolicy == TagStatePolicy::AnyDeclaredState || declaredShaderReadable ||
        (statePolicy == TagStatePolicy::AllowCommonTransition && diagnostic.state == 0u);
    if (!declaredStateAcceptable)
    {
        LOG_WARN("[RR_INPUT] {} is not declared NON_PIXEL_SHADER_RESOURCE (state={:#x}); refusing an untracked "
                 "external-state transition",
                 sourceName, diagnostic.state);
        return false;
    }

    const D3D12_RESOURCE_DESC desc = entry.resource->GetDesc();
    if (desc.Width != diagnostic.nativeWidth || desc.Height != diagnostic.nativeHeight ||
        desc.Format != diagnostic.format || desc.Dimension != diagnostic.dimension ||
        desc.DepthOrArraySize != diagnostic.arraySize || desc.MipLevels != diagnostic.mipLevels ||
        desc.SampleDesc.Count != diagnostic.sampleCount)
    {
        LOG_WARN("[RR_INPUT] {} resource description no longer matches its atomic tag metadata", sourceName);
        return false;
    }

    // Record legacy-tag consumption only after the tag has cleared every check.
    // Recording it earlier marks a tag consumed that we then reject, and the
    // staleness rule above would refuse that same updateCount on every later frame
    // unless the game happens to re-submit the tag.
    if (isLegacyTag)
    {
        _lastConsumedSLTagUpdates[index] = diagnostic.updateCount;
        _lastConsumedSLTagFrames[index] = activeFrame;
    }

    resource = entry.resource;
    return true;
}

bool FSRDFeatureDx12::AcquireTaggedAmbientOcclusionResources(bool logFailure)
{
    _ambientOcclusionNoisy.Reset();
    _ambientOcclusionDenoised.Reset();

    const RRD3D12SignalTagSnapshot snapshot = StreamlineHooks::getRRD3D12SignalTagSnapshot();
    RRTaggedResourceDiagnostic noisy {};
    RRTaggedResourceDiagnostic denoised {};
    Microsoft::WRL::ComPtr<ID3D12Resource> noisyResource;
    Microsoft::WRL::ComPtr<ID3D12Resource> denoisedResource;
    const bool acquiredNoisy =
        AcquireSLTaggedResource(snapshot, RRTaggedSignal::AmbientOcclusionNoisy, "Streamline.AmbientOcclusionNoisy",
                                TagStatePolicy::RequireShaderRead, noisyResource, noisy);
    const bool acquiredDenoised = AcquireSLTaggedResource(snapshot, RRTaggedSignal::AmbientOcclusionDenoised,
                                                          "Streamline.AmbientOcclusionDenoised",
                                                          TagStatePolicy::AnyDeclaredState, denoisedResource, denoised);

    const auto validMetadata = [this](const RRTaggedResourceDiagnostic& resource)
    {
        return resource.observed && resource.present && resource.dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
               resource.format == DXGI_FORMAT_R8_UNORM && resource.nativeWidth >= RenderWidth() &&
               resource.nativeHeight >= RenderHeight() && resource.effectiveWidth == RenderWidth() &&
               resource.effectiveHeight == RenderHeight() && resource.extentLeft == 0 && resource.extentTop == 0 &&
               resource.mipLevels == 1 && resource.arraySize == 1 && resource.sampleCount == 1 &&
               resource.state != UINT32_MAX;
    };

    if (!acquiredNoisy || !acquiredDenoised || !validMetadata(noisy) || !validMetadata(denoised))
    {
        if (logFailure)
        {
            LOG_WARN("[RR_AO] tagged AO requested but the noisy/denoised pair is not dispatch-safe. "
                     "noisyPresent={}, denoisedPresent={}, noisy={}x{} {}, denoised={}x{} {}, noisyState={:#x}. "
                     "Required: full-resolution R8_UNORM Texture2D resources and a shader-readable noisy input.",
                     noisy.present, denoised.present, noisy.effectiveWidth, noisy.effectiveHeight,
                     magic_enum::enum_name(noisy.format), denoised.effectiveWidth, denoised.effectiveHeight,
                     magic_enum::enum_name(denoised.format), noisy.state);
        }
        return false;
    }

    _ambientOcclusionNoisy = std::move(noisyResource);
    _ambientOcclusionDenoised = std::move(denoisedResource);

    _ambientOcclusionNoisyState = static_cast<D3D12_RESOURCE_STATES>(noisy.state);
    _ambientOcclusionDenoisedState = static_cast<D3D12_RESOURCE_STATES>(denoised.state);
    return true;
}

bool FSRDFeatureDx12::PublishAmbientOcclusionOutput(ID3D12GraphicsCommandList* commandList)
{
    if (!_ambientOcclusionEnabled)
        return true;

    if (!_ambientOcclusionDenoised)
    {
        LOG_ERROR("[RR_AO] tagged AO output disappeared before publication");
        return false;
    }

    return FSRDConvShader->CopyAmbientOcclusionOutput(commandList, _ambientOcclusionDenoised.Get(),
                                                      _ambientOcclusionDenoisedState, RenderWidth(), RenderHeight());
}

bool FSRDFeatureDx12::s_ngxDepthTypeSeen = false;
bool FSRDFeatureDx12::s_ngxReportedHWDepth = false;

bool FSRDFeatureDx12::InitFSR3(const NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    // Init upscaler first - borrow some init boilerplate and some cfg
    if (FSR31FeatureDx12::InitFSR3(InParameters))
    {
        SetInit(false);

        LOG_DEBUG("FSR Ray Regeneration Initializing");
        _name = "FSR-RR";

        if (int value; InParameters->Get(NVSDK_NGX_Parameter_Use_HW_Depth, &value) == NVSDK_NGX_Result_Success)
        {
            _hasNGXDepthType = true;
            _ngxReportedHWDepth = value == NVSDK_NGX_DLSS_Depth_Type_HW;
            s_ngxDepthTypeSeen = true;
            s_ngxReportedHWDepth = _ngxReportedHWDepth;
        }
        else if (s_ngxDepthTypeSeen)
        {
            // The depth type is a property of the title, not of one feature instance.
            // NGX only publishes it on a creation carrying DLSSD create params, so a
            // later recreation - a resolution or preset change, or a backend switch -
            // can arrive without it. Re-deriving per instance therefore loses a
            // declaration the title already made, and the resource inference below
            // then has to guess for the rest of the session.
            _hasNGXDepthType = true;
            _ngxReportedHWDepth = s_ngxReportedHWDepth;
            LOG_INFO("[RR_INPUT] {} absent on this creation; reusing the {} type this "
                     "title declared earlier",
                     NVSDK_NGX_Parameter_Use_HW_Depth, s_ngxReportedHWDepth ? "hardware" : "linear");
        }
        else
        {
            // NVSDK_NGX_DLSS_Depth_Type_Linear is zero, so a title that never fills
            // the field reads as linear. Most DLSS-RR titles actually supply a
            // hardware depth buffer, and interpreting one as a view-space distance
            // clamps the entire scene into [near, 1]: RR then sees a metre-deep
            // world, disocclusion and the depth delta stop working, and the linear
            // depth debug view goes black. Warn loudly and name the override.
            _hasNGXDepthType = false;
            _ngxReportedHWDepth = false;
            LOG_WARN("[RR_INPUT] title did not publish {}; the depth type will be inferred from the "
                     "depth resource, falling back to LINEAR if its format is ambiguous. If the "
                     "linear-depth debug view is black or geometry-dependent behaviour looks wrong, "
                     "set [FSR-RR] HardwareDepth=true in OptiScaler.ini or use Depth Input in the menu.",
                     NVSDK_NGX_Parameter_Use_HW_Depth);
        }
        _isHWDepth = _ngxReportedHWDepth;

        if (int value; InParameters->Get(NVSDK_NGX_Parameter_DLSS_Roughness_Mode, &value) == NVSDK_NGX_Result_Success)
        {
            if (value == NVSDK_NGX_DLSS_Roughness_Mode_Packed)
                _roughnessSource = RoughnessSource::Packed;
            else if (value == NVSDK_NGX_DLSS_Roughness_Mode_Unpacked)
                _roughnessSource = RoughnessSource::Separate;
            else
                LOG_WARN("Unknown DLSSD roughness mode {}; deferring source selection", value);
        }

        const char* roughnessSource =
            _roughnessSource == RoughnessSource::Packed
                ? "packed"
                : (_roughnessSource == RoughnessSource::Separate ? "separate" : "undetermined");
        LOG_INFO("DLSSD Flags HWDepth: {} (NGX reported: {}) - RoughnessSource: {}", _isHWDepth,
                 _hasNGXDepthType ? (_ngxReportedHWDepth ? "hardware" : "linear") : "absent", roughnessSource);

        _autoSpecularSignalDescType = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR;
        _autoSpecularSignalResolved = false;
        _autoDiffuseSignalDescType = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE;
        _autoDiffuseSignalResolved = false;

        if (!CreateDenoiserContext())
            return false;

        LOG_INFO("FSR Ray Regeneration Initialized");

        SetInit(true);
        return true;
    }

    return false;
}

bool FSRDFeatureDx12::CreateDenoiserContext()
{
    ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
    auto& state = State::Instance();
    const auto& cfg = *Config::Instance();

    if (!QueryDenoiserVersions())
        return false;

    InvalidateDenoiserHistory();

    const int requestedDenoiserIndex = cfg.FfxDenoiserIndex.value_or_default();
    const size_t denoiserIndex =
        std::clamp<size_t>(requestedDenoiserIndex < 0 ? 0u : static_cast<size_t>(requestedDenoiserIndex), 0u,
                           state.ffxDenoiserVersionIds.size() - 1u);

    if (denoiserIndex != static_cast<size_t>(std::max(requestedDenoiserIndex, 0)))
    {
        LOG_WARN("Configured RR provider index {} is unavailable; using provider index {}", requestedDenoiserIndex,
                 denoiserIndex);
    }

    const char* providerName =
        state.ffxDenoiserVersionNames[denoiserIndex] ? state.ffxDenoiserVersionNames[denoiserIndex] : "<unnamed>";

    // Parse the denoiser provider version into this instance's own field; the
    // SR upscaler version reported by Version() must stay untouched.
    _denoiserVersion.parse_version(providerName);

    _diffuseSignalDescType = GetDiffuseSignalDescType(cfg, _autoDiffuseSignalDescType);
    _specularSignalDescType = GetSpecularSignalDescType(cfg, _autoSpecularSignalDescType);
    // Single-signal mode: a disabled signal is neither dispatched nor declared at
    // context creation. Both disabled is a configuration error - fall back to both.
    _denoiseDiffuse = cfg.FfxDenoiserDenoiseDiffuse.value_or_default();
    _denoiseSpecular = cfg.FfxDenoiserDenoiseSpecular.value_or_default();
    if (!_denoiseDiffuse && !_denoiseSpecular)
    {
        LOG_WARN("FSR-RR DenoiseDiffuse and DenoiseSpecular are both false; "
                 "denoising both signals instead");
        _denoiseDiffuse = true;
        _denoiseSpecular = true;
    }
    _ambientOcclusionEnabled =
        cfg.FfxDenoiserTaggedAmbientOcclusion.value_or_default() && AcquireTaggedAmbientOcclusionResources(true);
    // RR 1.2 supports specular occlusion, and the preprocessor reserves an R8 output for it,
    // but Streamline exposes no semantic SO tag. Keep it source-gated until a real input exists.
    _specularOcclusionEnabled = false;

    uint32_t selectedSignalFlags = 0;
    if (_denoiseDiffuse)
        selectedSignalFlags |= GetSignalFlag(_diffuseSignalDescType);
    if (_denoiseSpecular)
        selectedSignalFlags |= GetSignalFlag(_specularSignalDescType);
    if (selectedSignalFlags == 0)
        selectedSignalFlags = GetSignalFlag(_diffuseSignalDescType) | GetSignalFlag(_specularSignalDescType);
    if (_ambientOcclusionEnabled)
        selectedSignalFlags |= FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION;

    ffxOverrideVersion vidOverride = { .header = { .type = FFX_API_DESC_TYPE_OVERRIDE_VERSION },
                                       .versionId = state.ffxDenoiserVersionIds[denoiserIndex] };
    // Create context
    // Backend desc
    ffxCreateBackendDX12Desc backendDesc = 
    { 
        .header = 
        { 
            .type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12,
            .pNext = &vidOverride.header // Chain override into backend desc
        },
        .device = Device
    };
    // Chain: ContextDesc -> BackendDesc -> OverrideVersion.
    // DLSS-RR exposes generic diffuse/specular lighting, while RR 1.2 requires
    // direct/indirect classification. The selected approximation is configurable.
    // Preserve the allocation ceiling when this context is recreated for a
    // signal-policy change while DRS is currently below its creation size.
    // Rounded up, because a preset change can land the render size a pixel past the one the context
    // was created with: the title and Streamline round the same ratio differently, and 1129x635
    // created against 1130x636 dispatched forced a full context rebuild -- and with it a temporal
    // history reset -- every time a preset moved. This field is an allocation ceiling and each
    // dispatch already supplies its own logical size below it, so the headroom simply absorbs that
    // jitter. An alignment of 8 costs well under a percent of the allocation.
    constexpr uint32_t kRenderCeilingAlignment = 8;
    const auto ceilingFor = [](uint32_t extent)
    { return (extent + kRenderCeilingAlignment - 1) / kRenderCeilingAlignment * kRenderCeilingAlignment; };

    const uint32_t maxRenderWidth = std::max(_denoiserCtxDesc.maxRenderSize.width, ceilingFor(RenderWidth()));
    const uint32_t maxRenderHeight = std::max(_denoiserCtxDesc.maxRenderSize.height, ceilingFor(RenderHeight()));
    _denoiserCtxDesc = { .header = { .type = FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER,
                                     // Chain backend desc into context desc
                                     .pNext = &backendDesc.header },
                         .version = FFX_DENOISER_VERSION,
                         .maxRenderSize = { maxRenderWidth, maxRenderHeight },
                         .signalFlags = selectedSignalFlags,
                         .checkerboardSignalFlags = FFX_DENOISER_SIGNAL_NONE,
                         .flags = 0 };

    if (cfg.FfxDenoiserInternalDebugViews.value_or_default())
    {
        _denoiserCtxDesc.flags |= FFX_DENOISER_ENABLE_DEBUGGING;
        LOG_INFO("[RR_DIAG] AMD internal debug views enabled for this denoiser context");
    }

#ifdef _DEBUG
    LOG_INFO("Debug views and validation enabled for denoiser!");
    _denoiserCtxDesc.flags |= FFX_DENOISER_ENABLE_DEBUGGING | FFX_DENOISER_ENABLE_VALIDATION;
#endif

    LOG_INFO("[RR_DIAG] creating context: providerIndex={}, providerName='{}' ({}.{}.{}), providerId={:#x}, "
             "api={}.{}.{}, maxRenderSize={}x{}, signalFlags={:#x}, checkerboardFlags={:#x}, createFlags={:#x}",
             denoiserIndex, providerName, _denoiserVersion.major, _denoiserVersion.minor, _denoiserVersion.patch,
             state.ffxDenoiserVersionIds[denoiserIndex], FFX_DENOISER_VERSION_MAJOR, FFX_DENOISER_VERSION_MINOR,
             FFX_DENOISER_VERSION_PATCH, _denoiserCtxDesc.maxRenderSize.width, _denoiserCtxDesc.maxRenderSize.height,
             _denoiserCtxDesc.signalFlags, _denoiserCtxDesc.checkerboardSignalFlags, _denoiserCtxDesc.flags);
    LOG_INFO("[RR_DIAG] signal classification: diffuse={}, specular={}, ambientOcclusion={}, "
             "specularOcclusion={} (no semantic source)",
             GetSignalTypeName(_diffuseSignalDescType), GetSignalTypeName(_specularSignalDescType),
             _ambientOcclusionEnabled, _specularOcclusionEnabled);
    spdlog::info(L"" __FUNCTIONW__ L" [RR_DIAG] denoiser module: {}", FfxApiProxy::Dx12Module_Denoiser_Path());

    // Create the denoiser context
    {
        ScopedSkipHeapCapture skipHeapCapture {};
        auto ret = FfxApiProxy::D3D12_CreateContext(&_pDenoiserCtx, &_denoiserCtxDesc.header, NULL);

        if (ret != FFX_API_RETURN_OK)
        {
            LOG_ERROR("_denoiserCtx error: {0}", FfxApiProxy::ReturnCodeToString(ret));
            return false;
        }

        LOG_INFO("[RR_DIAG] context creation succeeded: context={:X}", reinterpret_cast<uintptr_t>(_pDenoiserCtx));
    }

    // Query default settings
    if (!SetDefaultConfiguration())
    {
        LOG_ERROR("Failed to query the RR 1.2 default configuration");
        DestroyDenoiserContext();
        return false;
    }

    // The queried values are AMD's tuned baseline, but the per-frame configure pass
    // overwrites every one of them with the INI values. Keep a copy so the A/B switch
    // can push AMD's numbers back without a context recreation, and record both so a
    // temporal artefact can be attributed to - or cleared of - an over-aggressive
    // override without guessing what RR would have used on its own.
    _denoiserAmdDefaults = _denoiserSettings;

    LOG_INFO("[RR_DIAG] configure baseline (AMD default -> fork default), active source={}: "
             "disocclusionThreshold={:.4f} -> {:.4f}, crossBilateralNormalStrength={:.4f} -> {:.4f}, "
             "stabilityBias={:.4f} -> {:.4f}, maxRadiance={:.1f} -> {:.1f}, "
             "radianceClipStdK={:.4f} -> {:.4f}, gaussianKernelRelaxation={:.4f} -> {:.4f}",
             cfg.FfxDenoiserUseAmdDefaults.value_or_default() ? "AMD" : "fork",
             _denoiserSettings.m_DisocclusionThreshold, cfg.FfxDenoiserDisocThreshold.value_or_default(),
             _denoiserSettings.m_CrossBilateralNormalStrength, cfg.FfxDenoiserCrossBlNormStr.value_or_default(),
             _denoiserSettings.m_StabilityBias, cfg.FfxDenoiserStabilityBias.value_or_default(),
             _denoiserSettings.m_MaxRadiance, cfg.FfxDenoiserMaxRadiance.value_or_default(),
             _denoiserSettings.m_RadianceClipStdK, cfg.FfxDenoiserRadianceClip.value_or_default(),
             _denoiserSettings.m_GaussianKernelRelaxation, cfg.FfxDenoiserGaussKernRelax.value_or_default());

    // Create DLSS-RR to FSR-RR input converter
    auto newConverter = std::make_unique<FSRDPreprocessor_Dx12>("FSRD Converter", Device);

    if (!newConverter->IsInit())
    {
        LOG_ERROR("Failed to initialize the FSR-RR input converter");
        DestroyDenoiserContext();
        return false;
    }

    if (!newConverter->SetMaxRenderSize(_denoiserCtxDesc.maxRenderSize.width, _denoiserCtxDesc.maxRenderSize.height))
    {
        LOG_ERROR("Failed to allocate the FSR-RR input converter");
        DestroyDenoiserContext();
        return false;
    }

    FSRDConvShader = std::move(newConverter);

    // The converter is brand new, so nothing in flight can reference its resources.
    _preprocessorHasRecordedWork = false;
    _logNextDenoiserDispatch = true;
    _lastDispatchRequestedReset = false;

    return true;
}

bool FSRDFeatureDx12::QueryDenoiserVersions()
{
    ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
    auto& state = State::Instance();

    // Get version count
    uint64_t versionCount = 0;
    ffxQueryDescGetVersions queryVersionsDesc = { .header = { .type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS },
                                                  .createDescType = FFX_API_EFFECT_ID_DENOISER,
                                                  .device = Device,
                                                  .outputCount = &versionCount };
    const ffxReturnCode_t countResult = FfxApiProxy::D3D12_Query(nullptr, &queryVersionsDesc.header);

    if (countResult != FFX_API_RETURN_OK)
    {
        LOG_ERROR("Failed to query RR provider count: {}", FfxApiProxy::ReturnCodeToString(countResult));
        return false;
    }

    state.ffxDenoiserVersionIds.resize(versionCount);
    state.ffxDenoiserVersionNames.resize(versionCount);

    state.ffxDenoiserDebugModes.clear();
    state.ffxDenoiserDebugModeNames.clear();

    for (const auto& mode : kDebugModes)
    {
        state.ffxDenoiserDebugModes.push_back(mode.second);
        state.ffxDenoiserDebugModeNames.emplace(mode.second, mode.first);
    }

    if (versionCount == 0)
    {
        LOG_ERROR("No FSR-RR denoisers were found.");
        return false;
    }
    else
        LOG_DEBUG("Found {} versions of FSR-RR", versionCount);

    LOG_DEBUG("Initialising FSR denoiser context");

    // Get version IDs
    queryVersionsDesc.versionIds = state.ffxDenoiserVersionIds.data();
    queryVersionsDesc.versionNames = state.ffxDenoiserVersionNames.data();
    const ffxReturnCode_t versionsResult = FfxApiProxy::D3D12_Query(nullptr, &queryVersionsDesc.header);
    if (versionsResult != FFX_API_RETURN_OK)
    {
        LOG_ERROR("Failed to query RR providers: {}", FfxApiProxy::ReturnCodeToString(versionsResult));
        return false;
    }

    for (size_t i = 0; i < state.ffxDenoiserVersionIds.size(); ++i)
    {
        LOG_INFO("[RR_DIAG] provider[{}]: name='{}', id={:#x}", i,
                 state.ffxDenoiserVersionNames[i] ? state.ffxDenoiserVersionNames[i] : "<unnamed>",
                 state.ffxDenoiserVersionIds[i]);
    }

    return true;
}

void FSRDFeatureDx12::DestroyDenoiserContext()
{
    // The deferred-destruction queue guarantees this is only reached with all recordings
    // retired (or at process shutdown). Direct destruction with outstanding recordings
    // would be a lifetime bug; say so loudly instead of silently releasing.
    if (!RecordingsRetired() && !State::Instance().isShuttingDown)
        LOG_ERROR("FSR-RR context destroyed while {} recording(s) are still outstanding",
                  _recordingTickets.size());

    if (_pDenoiserCtx != nullptr)
    {
        const uintptr_t contextAddress = reinterpret_cast<uintptr_t>(_pDenoiserCtx);
        const ffxReturnCode_t result = FfxApiProxy::D3D12_DestroyContext(&_pDenoiserCtx, nullptr);

        if (result == FFX_API_RETURN_OK)
        {
            LOG_INFO("[RR_DIAG] context destruction succeeded: context={:X}", contextAddress);
        }
        else
        {
            LOG_ERROR("[RR_DIAG] context destruction failed: context={:X}, result={}", contextAddress,
                      FfxApiProxy::ReturnCodeToString(result));
        }
    }

    _pDenoiserCtx = nullptr;
    _ambientOcclusionEnabled = false;
    _specularOcclusionEnabled = false;
    _ambientOcclusionNoisy.Reset();
    _ambientOcclusionDenoised.Reset();
    InvalidateDenoiserHistory();
}

bool FSRDFeatureDx12::UpdateSize()
{
    const uint32_t renderWidth = RenderWidth();
    const uint32_t renderHeight = RenderHeight();
    const uint32_t maxWidth = _denoiserCtxDesc.maxRenderSize.width;
    const uint32_t maxHeight = _denoiserCtxDesc.maxRenderSize.height;

    // maxRenderSize is an allocation ceiling; each dispatch supplies the current
    // logical size. Never release context resources while recording a frame because
    // earlier submitted command lists may still reference them.
    if (renderWidth > maxWidth || renderHeight > maxHeight)
    {
        LOG_ERROR("[RR_DIAG] render size {}x{} exceeds the FSR-RR creation ceiling {}x{}; requesting feature "
                  "recreation instead of releasing in-flight resources",
                  renderWidth, renderHeight, maxWidth, maxHeight);
        InvalidateDenoiserHistory();
        State::Instance().changeBackend[Handle()->Id] = true;
        return false;
    }

    if (_lastDenoiserRenderWidth != 0 &&
        (_lastDenoiserRenderWidth != renderWidth || _lastDenoiserRenderHeight != renderHeight))
    {
        LOG_INFO("[RR_DIAG] logical render size changed within the allocation ceiling: {}x{} -> {}x{}; resetting "
                 "temporal history without recreating resources",
                 _lastDenoiserRenderWidth, _lastDenoiserRenderHeight, renderWidth, renderHeight);
        InvalidateDenoiserHistory();
    }

    _lastDenoiserRenderWidth = renderWidth;
    _lastDenoiserRenderHeight = renderHeight;

    return true;
}

bool FSRDFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (!IsInited())
        return false;

    // Whole-instance retirement: stamp this evaluate's command list (the preprocessor
    // slots track themselves; the denoiser and upscaler dispatches ride this list too).
    // Outstanding tickets keep the instance alive through DestroyWhenRetired.
    {
        auto& tracker = DlssNr::Submission::GpuBatch::Tracker::Instance();
        std::erase_if(_recordingTickets, [&tracker](uint64_t ticket) { return tracker.Retired(ticket); });
        _recordingTickets.push_back(tracker.Record(InCommandList));
        FSRDFeatureDx12::ReapDeferredFeatures();
    }

    // The application submitted a new evaluation even when a later validation,
    // composition, or upscaler step fails. Keep RR frame indices unique on every
    // exit path; in particular, never reuse an index after RR already advanced
    // its internal temporal history.
    EvaluationFrameGuard evaluationFrameGuard(_frameCount);

    auto& state = State::Instance();
    auto& cfg = *Config::Instance();
    const auto& inParams = *InParameters;

    // Refresh the current render subrect before deciding whether RR must resize.
    // PrepareUpscalerInput queries it too, but that runs after this decision, so the
    // resize would otherwise be made against the previous frame's extent.
    unsigned int currentRenderWidth = RenderWidth();
    unsigned int currentRenderHeight = RenderHeight();
    GetRenderResolution(InParameters, &currentRenderWidth, &currentRenderHeight);
    if (currentRenderWidth == 0 || currentRenderHeight == 0)
    {
        LOG_ERROR("[RR_INPUT] current render resolution is zero");
        InvalidateDenoiserHistory();
        return false;
    }

    if (!UpdateSize())
        return false;

    StreamlineHooks::probeRRNGXPointerParameters(inParams);

    // Conversion leaves the RR signal outputs in UAV state. Always close that
    // state lifetime, including bypass and error paths where composition is skipped.
    DenoiserOutputStateGuard denoiserOutputStateGuard(FSRDConvShader, InCommandList);
    TitleInputStateGuard titleInputStateGuard(FSRDConvShader, InCommandList);

    const auto dbgMode = static_cast<DebugModes>(cfg.FfxDenoiserDebugMode.value_or_default());
    const bool isDebugVis = (uint32_t) dbgMode & (uint32_t) DebugModes::ConversionDebug;
    const bool isDebugComp = ((uint64_t) dbgMode & (uint64_t) DebugModes::CompositionDebug);
    const bool isFfxDebug = dbgMode == DebugModes::FfxDebug;
    const bool isAmbientOcclusionDebug =
        dbgMode == DebugModes::AmbientOcclusionInput || dbgMode == DebugModes::AmbientOcclusionOutput;
    const bool hasAnyDebug = (dbgMode != DebugModes::None);
    _convDesc.FrameIndex = static_cast<uint64_t>(_frameCount);

    // Denoise is bypassed if we are debugging something OTHER than the final outputs
    const bool isDenoiseBypassed = !isFfxDebug && !isDebugComp && hasAnyDebug && !isAmbientOcclusionDebug &&
                                   dbgMode != DebugModes::DenoiserOutput && dbgMode != DebugModes::UpscalerBypass;

    // Upscale is bypassed if we are in a debug mode that isn't the DenoiserBypass (final raw)
    const bool isUpscaleBypassed = hasAnyDebug && dbgMode != DebugModes::DenoiserBypass;

    _isInReset = false;

    if (uint32_t value = 0; inParams.Get(NVSDK_NGX_Parameter_Reset, &value) == NVSDK_NGX_Result_Success)
        _isInReset = value > 0;

    // The conversion reads the title's color, depth and motion vectors in its very first
    // dispatch, so those inputs have to be acquired and taken out of their declared starting
    // states before the denoiser chain begins - acquiring them inside the upscaler branch
    // below would leave the floor and packing passes reading render-target and unordered-access
    // resources as if they were shader resources.
    ffxDispatchDescUpscale upscalerDesc = {};
    if (!PrepareUpscalerInput(InCommandList, inParams, upscalerDesc))
    {
        InvalidateDenoiserHistory();
        return false;
    }

    // Optional, configurable resource barriers. The window spans the whole chain and closes on
    // every exit path, including the debug bypass that never reaches the upscaler dispatch, so
    // the title finds each resource in the state it declared.
    FSR31FeatureDx12::ScopedConfigurableBarriers scopedBarriers(*this, InCommandList);

    // Denoiser start
    ffxDispatchDescDenoiserAmbientOcclusion ambientOcclusion = {};
    ffxDispatchDescDenoiserDirectDiffuse directDiffuse = {};
    ffxDispatchDescDenoiserIndirectSpecular indirectSpecular = {};
    ffxDispatchDescDenoiser denoiserDesc = {};
    bool isDenoiserReady = false;

    // Pull configuration and input buffers for DLSS-RR from the param table, convert and
    // repack input buffers into intermediate FSR-RR input buffers, and configure descriptors.
    if (!PrepareDenoiserInput(InCommandList, *InParameters, denoiserDesc, ambientOcclusion, directDiffuse,
                              indirectSpecular))
    {
        InvalidateDenoiserHistory();
        return false;
    }

    // Dispatch denoiser
    if (!isDenoiseBypassed)
    {
        ffxDispatchDescDenoiserDebugView dispatchDebugView = {};

        if (isFfxDebug)
        {
            if (!(_denoiserCtxDesc.flags & FFX_DENOISER_ENABLE_DEBUGGING))
            {
                LOG_ERROR("RR debug view requested, but this denoiser context was not created with debugging enabled");
                InvalidateDenoiserHistory();
                return false;
            }

            ID3D12Resource* debugOutput =
                FSRDConvShader->PrepareDebugViewOutput(InCommandList, TargetWidth(), TargetHeight());
            if (!debugOutput)
            {
                InvalidateDenoiserHistory();
                return false;
            }

            const D3D12_RESOURCE_DESC debugOutputDesc = debugOutput->GetDesc();
            const bool isCompatibleDebugOutput =
                debugOutputDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                debugOutputDesc.Width == TargetWidth() && debugOutputDesc.Height == TargetHeight() &&
                debugOutputDesc.DepthOrArraySize == 1 && debugOutputDesc.MipLevels == 1 &&
                debugOutputDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && debugOutputDesc.SampleDesc.Count == 1 &&
                (debugOutputDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
            if (!isCompatibleDebugOutput)
            {
                LOG_ERROR("[RR_DIAG] dedicated AMD debug-view target is incompatible: size={}x{}, "
                          "format={}, dimension={}, arraySize={}, mips={}, samples={}, flags={:#x}",
                          debugOutputDesc.Width, debugOutputDesc.Height, static_cast<uint32_t>(debugOutputDesc.Format),
                          static_cast<uint32_t>(debugOutputDesc.Dimension), debugOutputDesc.DepthOrArraySize,
                          debugOutputDesc.MipLevels, debugOutputDesc.SampleDesc.Count,
                          static_cast<uint32_t>(debugOutputDesc.Flags));
                InvalidateDenoiserHistory();
                return false;
            }

            const int debugViewport = std::clamp(cfg.FfxDenoiserDebugViewport.value_or_default(), -1,
                                                 FFX_API_DENOISER_DEBUG_VIEW_MAX_VIEWPORTS - 1);
            dispatchDebugView = {
                .header = { .type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DEBUG_VIEW },
                .output = ffxApiGetResourceDX12(debugOutput, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS),
                .outputSize = { static_cast<uint32_t>(debugOutputDesc.Width), debugOutputDesc.Height },
                .mode = static_cast<uint32_t>(debugViewport < 0 ? FFX_API_DENOISER_DEBUG_VIEW_MODE_OVERVIEW
                                                                : FFX_API_DENOISER_DEBUG_VIEW_MODE_FULLSCREEN_VIEWPORT),
                .viewportIndex = static_cast<uint32_t>(std::max(debugViewport, 0))
            };

            // Debug view is optional and must be the tail of the typed-signal chain.
            ffxDispatchDescHeader* chainTail = denoiserDesc.header.pNext;
            while (chainTail && chainTail->pNext)
                chainTail = chainTail->pNext;

            if (!chainTail)
            {
                LOG_ERROR("RR 1.2 debug view could not find the typed-signal chain tail");
                InvalidateDenoiserHistory();
                return false;
            }

            chainTail->pNext = &dispatchDebugView.header;
        }

        isDenoiserReady = DispatchDenoiser(InCommandList, denoiserDesc);

        if (isFfxDebug)
            FSRDConvShader->TransitionDebugViewOutputToRead(InCommandList);

        if (!isDenoiserReady)
        {
            InvalidateDenoiserHistory();
            return false;
        }

        if (!PublishAmbientOcclusionOutput(InCommandList))
        {
            InvalidateDenoiserHistory();
            return false;
        }

        CommitDenoiserHistory();

        // Compose denoised signals
        uint32_t compositionFlags = (uint32_t) GetCompDebugFlags(dbgMode);
        if (_diffuseSignalDescType == FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE)
            compositionFlags |= (uint32_t) FSRDCompFlags::DiffuseSignalIndirect;
        if (_specularSignalDescType == FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR)
            compositionFlags |= (uint32_t) FSRDCompFlags::SpecularSignalIndirect;
        // A signal left out of the denoiser chain has no output this frame, and the
        // buffer it would have been written to is one of the floor passes' ping-pong
        // targets. Flag it so composition reads the raw signal rather than the floor
        // image that buffer still holds.
        if (!_denoiseDiffuse)
            compositionFlags |= (uint32_t) FSRDCompFlags::DiffuseSignalDisabled;
        if (!_denoiseSpecular)
            compositionFlags |= (uint32_t) FSRDCompFlags::SpecularSignalDisabled;

        FSRDCompDesc compDesc = {
            .DstTexSize = _convDesc.RenderSize,
            .CorrelationBias = std::clamp(cfg.FfxDenoiserCorrelationBias.value_or_default(), 0.0f, 1.0f),
            .Flags = compositionFlags,
            .FloorHandoverAnchorClamp =
                std::clamp(cfg.FfxDenoiserFloorHandoverAnchorClamp.value_or_default(), 0.0f, 4.0f),
            .FloorHandoverCorrelationMix =
                std::clamp(cfg.FfxDenoiserFloorHandoverCorrelationMix.value_or_default(), 0.0f, 1.0f)
        };

        const XMUINT2 rawColorBase = GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
                                                    NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y);
        compDesc.SourceBase = { rawColorBase.x, rawColorBase.y, 0, 0 };

        if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_Color, compDesc.InRawColor) ||
            !ValidateSourceExtent("CompositionColor", compDesc.InRawColor, rawColorBase, RenderWidth(), RenderHeight()))
        {
            InvalidateDenoiserHistory();
            return false;
        }

        // ColorBeforeParticles is a whole scene guide, not a premultiplied overlay.
        // It is deliberately absent from composition; the title's Color input already
        // contains the scene contribution that reaches the final frame.

        if (!isFfxDebug)
        {
            if (!FSRDConvShader->DispatchComposition(InCommandList, compDesc))
                return false;
        }

        isDenoiserReady = true;
    }
    else
    {
        // A skipped RR frame breaks temporal continuity. The next real dispatch
        // must reset instead of reusing history across the gap.
        InvalidateDenoiserHistory();
    }

    // Upscaler start. Stays true on the debug/bypass paths where no upscale is requested.
    bool isUpscalerReady = true;

    if (!isUpscaleBypassed)
    {
        // Override upscaler config. The composition output is left in
        // NON_PIXEL | PIXEL shader-resource state, so declare both: the default
        // argument is COMPUTE_READ alone, which would leave the resource in a
        // narrower state than the preprocessor expects on the next frame.
        if (isDenoiserReady)
            upscalerDesc.color = ffxApiGetResourceDX12(FSRDConvShader->GetCompositionOutput(),
                                                       FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        // NR between the denoise and the upscale (the before-SR placement under FSR-RR): the model
        // sees the composed colour, denoised and still at render resolution, and SR temporally
        // accumulates the enhanced frame. Honours [DlssNr] RunBeforeSR and ApplyAfterRR: RR frames
        // run NR at exactly one seam or none, and ApplyAfterRR=false must leave the RR+SR output
        // untouched at BOTH seams (the after-upscale seam declines on its own).
        if (isDenoiserReady && !isFfxDebug && Config::Instance()->DlssNrEnabled.value_or_default() &&
            cfg.DlssNrApplyAfterRR.value_or_default())
        {
            constexpr auto composedState =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            const auto handOver = cfg.ColorResourceBarrier.has_value()
                                      ? static_cast<D3D12_RESOURCE_STATES>(cfg.ColorResourceBarrier.value())
                                      : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            ID3D12Resource* composed = FSRDConvShader->GetCompositionOutput();

            // Re-arm the proxy's layout book before NR reads these inputs: the FFX dispatch
            // chain managed them with enhanced barriers, which tombstones their entries. FFX
            // restores each input to the read state this feature declared in its dispatch
            // descriptions, so that contract is authoritative here, not the tombstone.
            if (Microsoft::WRL::ComPtr<DlssNr::Submission::IRecordingResources> recorder;
                SUCCEEDED(InCommandList->QueryInterface(IID_PPV_ARGS(&recorder))))
            {
                ID3D12Resource* bookInputs[2] = {};
                D3D12_BARRIER_LAYOUT bookLayouts[2] = {};
                UINT bookCount = 0;
                ID3D12Resource* motion = nullptr;
                ID3D12Resource* depth = nullptr;
                if (TryGetNGXVoidPointer(*InParameters, NVSDK_NGX_Parameter_MotionVectors, motion) && motion)
                {
                    bookInputs[bookCount] = motion;
                    bookLayouts[bookCount] = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
                    ++bookCount;
                }
                if (TryGetNGXVoidPointer(*InParameters, NVSDK_NGX_Parameter_Depth, depth) && depth && bookCount < 2)
                {
                    bookInputs[bookCount] = depth;
                    bookLayouts[bookCount] = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
                    ++bookCount;
                }
                if (bookCount)
                    recorder->DeclareInputLayouts(bookCount, bookInputs, bookLayouts);
            }
 
            ResourceBarrier(InCommandList, composed, composedState, handOver);
            ID3D12Resource* edited = DlssNr::EvaluateAfterDenoise(InCommandList, InParameters, composed);
            ResourceBarrier(InCommandList, composed, handOver, composedState);

            if (edited != nullptr)
                upscalerDesc.color = ffxApiGetResourceDX12(edited, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        }

        isUpscalerReady = DispatchUpscaler(InCommandList, upscalerDesc);

        // Post-processing (RCAS/output scaling/overlay) is run by IFeature_Dx12::Evaluate.
    }
    else // Debug visualization
    {
        ID3D12Resource* srcTex = nullptr;
        XMUINT2 debugSourceBase {};
        XMFLOAT2 debugSourceLogicalSize { static_cast<float>(RenderWidth()), static_cast<float>(RenderHeight()) };

        if (isFfxDebug)
        {
            srcTex = FSRDConvShader->GetDebugViewOutput();
            debugSourceLogicalSize = { static_cast<float>(TargetWidth()), static_cast<float>(TargetHeight()) };
        }
        else if (dbgMode == DebugModes::DlssColorBeforeParticles)
        {
            TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles, srcTex);
            debugSourceBase = GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles_Subrect_Base_X,
                                             NVSDK_NGX_Parameter_DLSSD_ColorBeforeParticles_Subrect_Base_Y);
        }
        else if (dbgMode == DebugModes::DlssColorBeforeTransparency)
        {
            TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency, srcTex);
            debugSourceBase = GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency_Subrect_Base_X,
                                             NVSDK_NGX_Parameter_DLSSD_ColorBeforeTransparency_Subrect_Base_Y);
        }
        else if (dbgMode == DebugModes::DlssTransparencyLayer)
        {
            TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSS_TransparencyLayer, srcTex);
            debugSourceBase = GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_X,
                                             NVSDK_NGX_Parameter_DLSS_TransparencyLayer_Subrect_Base_Y);
        }
        else if (dbgMode == DebugModes::DlssBias)
        {
            TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, srcTex);
            debugSourceBase = GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X,
                                             NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y);
        }
        else if (dbgMode == DebugModes::RawColor)
        {
            TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_Color, srcTex);
            debugSourceBase = GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
                                             NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y);
        }
        else if (dbgMode == DebugModes::AmbientOcclusionInput)
            srcTex = _ambientOcclusionEnabled ? _ambientOcclusionNoisy.Get() : nullptr;
        else if (dbgMode == DebugModes::AmbientOcclusionOutput)
            srcTex = _ambientOcclusionEnabled ? FSRDConvShader->GetAmbientOcclusionOutput() : nullptr;
        else if (isDebugVis)
            srcTex = GetD3D12ResFromFFX(indirectSpecular.signal.input);
        else
            srcTex = FSRDConvShader->GetCompositionOutput();

        ID3D12Resource* dstTex;

        if (!srcTex || !TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_Output, dstTex))
            return true;

        FSRDConvShader->Blit(InCommandList, srcTex, dstTex, {}, debugSourceLogicalSize,
                             { static_cast<float>(debugSourceBase.x), static_cast<float>(debugSourceBase.y) });
    }

    // A failed upscale dispatch leaves the frame half finished. Report it to the caller
    // instead of masking it behind the denoiser result, which may well be true.
    if (!isUpscalerReady)
        return false;

    return isDenoiserReady || isDenoiseBypassed;
}

bool FSRDFeatureDx12::PrepareDenoiserInput(ID3D12GraphicsCommandList* InCommandList,
                                           const NVSDK_NGX_Parameter& inParams, ffxDispatchDescDenoiser& dispatchDesc,
                                           ffxDispatchDescDenoiserAmbientOcclusion& ambientOcclusion,
                                           ffxDispatchDescDenoiserDirectDiffuse& directDiffuse,
                                           ffxDispatchDescDenoiserIndirectSpecular& indirectSpecular)
{
    const auto& cfg = *Config::Instance();

    if (_ambientOcclusionEnabled && !AcquireTaggedAmbientOcclusionResources(true))
    {
        LOG_ERROR("[RR_AO] context requires AO, but a valid tagged pair was unavailable for this frame");
        return false;
    }

    // Gather DLSS-RR input buffers for conversion and repacking for FSR-RR
    if (!PrepareDenoiseConvInput(inParams))
        return false;

    if (!ConvertDenoiserBuffers(InCommandList))
        return false;

    // Camera matrix - translation and rotation, from viewMatrix^-1
    const XMFLOAT3 camPos = GetFloat3Column(_invViewMatrix, 3);
    const bool resetHistory = _isInReset || !_hasDenoiserHistory;
    const XMFLOAT3 camDelta =
        resetHistory ? XMFLOAT3 {}
                     : XMFLOAT3 { _lastCamPos.x - camPos.x, _lastCamPos.y - camPos.y, _lastCamPos.z - camPos.z };

    // Pack dispatch configuration
    dispatchDesc = { .commandList = InCommandList,
                     .motionVectorScale = { 1.0f, 1.0f, 1.0f },
                     // Camera movement since last frame (PreviousPosition - CurrentPosition)
                     .cameraPositionDelta = { camDelta.x, camDelta.y, camDelta.z },
                     .view = GetRRMatrix(_viewMatrix),
                     .projection = GetRRMatrix(_projMatrix),
                     .linearDepthBounds = { _convDesc.NearPlane, _convDesc.FarPlane },
                     .renderSize = { RenderWidth(), RenderHeight() },
                     .frameIndex = (uint32_t) _frameCount,
                     .flags = FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO };

    // Populate resources and link signal header
    FSRDConvShader->GetSignals(dispatchDesc, directDiffuse, indirectSpecular);
    directDiffuse.header.type = _diffuseSignalDescType;
    indirectSpecular.header.type = _specularSignalDescType;

    const ffxDispatchDescDenoiserAmbientOcclusion* activeAmbientOcclusion = nullptr;
    if (_ambientOcclusionEnabled)
    {
        // Acquisition only guarantees NON_PIXEL_SHADER_RESOURCE. Declaring the wider
        // PIXEL_COMPUTE_READ when the title tagged a narrower state makes the FFX
        // backend record a transition whose "before" state the resource is not in.
        // Report the state that was actually validated.
        const uint32_t ambientOcclusionInputState =
            (_ambientOcclusionNoisyState & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0
                ? FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ
                : FFX_API_RESOURCE_STATE_COMPUTE_READ;

        ambientOcclusion = { .header = { .type = FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION },
                             .signal = { .input = ffxApiGetResourceDX12(_ambientOcclusionNoisy.Get(),
                                                                        ambientOcclusionInputState),
                                         .output = ffxApiGetResourceDX12(FSRDConvShader->GetAmbientOcclusionOutput(),
                                                                         FFX_API_RESOURCE_STATE_UNORDERED_ACCESS),
                                         .checkerboardOrigin = 0 } };
        activeAmbientOcclusion = &ambientOcclusion;
    }

    // RR 1.2 descriptors are ordered by their ABI type, matching AMD's sample chain.
    // Keeping this generic prevents future AO/SO additions from hard-coding pairwise links.
    // Single-signal mode (DenoiseDiffuse/DenoiseSpecular): disabled signals are not
    // linked into the chain at all, so the denoiser only sees the enabled ones.
    std::array<ffxDispatchDescHeader*, 3> signals { nullptr, nullptr, nullptr };
    size_t signalCount = 0;
    if (_denoiseDiffuse)
        signals[signalCount++] = &directDiffuse.header;
    if (_denoiseSpecular)
        signals[signalCount++] = &indirectSpecular.header;
    if (activeAmbientOcclusion)
        signals[signalCount++] = &ambientOcclusion.header;

    std::sort(signals.begin(), signals.begin() + signalCount,
              [](const ffxDispatchDescHeader* left, const ffxDispatchDescHeader* right)
              { return left->type < right->type; });

    dispatchDesc.header.pNext = signalCount > 0 ? signals[0] : nullptr;
    for (size_t i = 0; i < signalCount; ++i)
        signals[i]->pNext = i + 1 < signalCount ? signals[i + 1] : nullptr;

    if (!ValidateRequiredRRResources(dispatchDesc, directDiffuse, indirectSpecular, activeAmbientOcclusion))
        return false;

    if (resetHistory)
        dispatchDesc.flags |= FFX_DENOISER_DISPATCH_RESET;

    // Conversion has already normalized source motion into unjittered UV space.
    // RR therefore consumes XY with a unit scale; Z remains signed-linear depth.
    dispatchDesc.motionVectorScale = { 1.0f, 1.0f, 1.0f };
    dispatchDesc.jitterOffsets.x = _convDesc.JitterOffsets.x;
    dispatchDesc.jitterOffsets.y = _convDesc.JitterOffsets.y;

    LOG_DEBUG("Jitter pixels [{:.6f}, {:.6f}]", dispatchDesc.jitterOffsets.x, dispatchDesc.jitterOffsets.y);

    return true;
}

// Picks the specular ray length RR will read, from the title's candidates.
//
// Selection is deferred until every source origin and extent is known, so an
// invalid high-priority candidate can never reach an SRV binding.
void FSRDFeatureDx12::ResolveSpecularHitDistance(const NVSDK_NGX_Parameter& inParams,
                                                 const RRD3D12SignalTagSnapshot& rrTagSnapshot, uint32_t renderWidth,
                                                 uint32_t renderHeight, uint32_t motionWidth, uint32_t motionHeight,
                                                 ID3D12Resource* ngxSpecularHitDistance,
                                                 ID3D12Resource* ngxSpecularRayDirectionHitDistance,
                                                 const XMUINT2& ngxSpecularHitDistanceBase,
                                                 const XMUINT2& ngxSpecularRayDirectionHitDistanceBase)
{
    struct SpecularHitDistanceSelection
    {
        ID3D12Resource* Resource = nullptr;
        XMUINT2 Base {};
        bool CombinedAlpha = false;
        const char* SourceName = nullptr;
    } specularHitDistanceSelection;

    auto tryNGXSpecularHitDistance = [&](const char* sourceName, ID3D12Resource* resource, const XMUINT2& base,
                                         bool combinedAlpha) -> bool
    {
        if (!resource)
            return false;

        const SourceFormatValidator formatValidator =
            combinedAlpha ? IsDiffuseRayDirectionHitDistanceFormat : IsDiffuseHitDistanceFormat;
        const char* expectedFormat = combinedAlpha ? "RGBA16F or RGBA32F (hit distance in alpha)" : "R16F or R32F";
        if (!ValidateReprojectionGuideSource(sourceName, resource, base, renderWidth, renderHeight, formatValidator,
                                             expectedFormat))
        {
            return false;
        }

        specularHitDistanceSelection = { resource, base, combinedAlpha, sourceName };
        return true;
    };

    auto trySLSpecularHitDistance = [&](RRTaggedSignal signal, const char* sourceName, bool combinedAlpha) -> bool
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> taggedResource;
        RRTaggedResourceDiagnostic diagnostic {};
        if (!AcquireSLTaggedResource(rrTagSnapshot, signal, sourceName, TagStatePolicy::RequireShaderRead,
                                     taggedResource, diagnostic))
            return false;

        const XMUINT2 base =
            diagnostic.usesExtent ? XMUINT2 { diagnostic.extentLeft, diagnostic.extentTop } : XMUINT2 {};
        if (diagnostic.effectiveWidth != renderWidth || diagnostic.effectiveHeight != renderHeight)
        {
            LOG_ERROR("[RR_INPUT] {} tag extent {}x{} does not exactly match the one-to-one render extent {}x{}",
                      sourceName, diagnostic.effectiveWidth, diagnostic.effectiveHeight, renderWidth, renderHeight);
            return false;
        }

        const SourceFormatValidator formatValidator =
            combinedAlpha ? IsDiffuseRayDirectionHitDistanceFormat : IsDiffuseHitDistanceFormat;
        const char* expectedFormat = combinedAlpha ? "RGBA16F or RGBA32F (hit distance in alpha)" : "R16F or R32F";
        if (!ValidateReprojectionGuideSource(sourceName, taggedResource.Get(), base, diagnostic.effectiveWidth,
                                             diagnostic.effectiveHeight, formatValidator, expectedFormat))
        {
            return false;
        }

        specularHitDistanceSelection = { taggedResource.Get(), base, combinedAlpha, sourceName };
        if (combinedAlpha)
            _specularRayDirectionHitDistanceTaggedResource = std::move(taggedResource);
        else
            _specularHitDistanceTaggedResource = std::move(taggedResource);
        return true;
    };

    if (!tryNGXSpecularHitDistance(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance, ngxSpecularHitDistance,
                                   ngxSpecularHitDistanceBase, false) &&
        !tryNGXSpecularHitDistance(NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance,
                                   ngxSpecularRayDirectionHitDistance, ngxSpecularRayDirectionHitDistanceBase, true) &&
        !trySLSpecularHitDistance(RRTaggedSignal::SpecularHitDistance, "Streamline.SpecularHitDistance", false))
    {
        trySLSpecularHitDistance(RRTaggedSignal::SpecularRayDirectionHitDistance,
                                 "Streamline.SpecularRayDirectionHitDistance", true);
    }

    if (specularHitDistanceSelection.Resource)
    {
        _convDesc.SpecularHitDistanceBase = specularHitDistanceSelection.Base;
        _convDesc.SpecularHitDistanceFromCombinedAlpha = specularHitDistanceSelection.CombinedAlpha;
        if (specularHitDistanceSelection.CombinedAlpha)
        {
            _convDesc.Resources.InSpecularRayDirectionHitDistance = specularHitDistanceSelection.Resource;
        }
        else
        {
            _convDesc.Resources.InSpecHitDist = specularHitDistanceSelection.Resource;
            // The legacy signal splitter consumes scalar hit distance directly.
            _convDesc.InputBase2.x = specularHitDistanceSelection.Base.x;
            _convDesc.InputBase2.y = specularHitDistanceSelection.Base.y;
        }

        LOG_DEBUG("[RR_INPUT] selected {} for specular hit distance: base=({}, {}), combinedAlpha={}",
                  specularHitDistanceSelection.SourceName, specularHitDistanceSelection.Base.x,
                  specularHitDistanceSelection.Base.y, specularHitDistanceSelection.CombinedAlpha);
    }
}

// Binds the resources a title may publish beyond the required set.
//
// Each is validated before use: presence says nothing about usability, and a
// sentinel-filled resource passes every presence check there is.
void FSRDFeatureDx12::AcquireOptionalInputs(const NVSDK_NGX_Parameter& inParams,
                                            const RRD3D12SignalTagSnapshot& rrTagSnapshot, uint32_t renderWidth,
                                            uint32_t renderHeight)
{
    const auto& cfg = *Config::Instance();

    // Optional title-published inputs. Each is validated before use: presence says nothing
    // about usability, and a sentinel-filled resource passes every presence check there is.
    _convDesc.Resources.InTitleLinearDepth = nullptr;
    _convDesc.Resources.InResponsivityMask = nullptr;
    _convDesc.TitleLinearDepthBase = {};
    _convDesc.TitleLinearDepthState = 0;
    _titleLinearDepthTaggedResource.Reset();
    _responsivityMaskTaggedResource.Reset();

    // The title's own linear depth. Opt-in: every consumer of view-space position switches
    // to it at once, so it is not a change to make by default before it has been compared
    // against the derived field.
    if (cfg.FfxDenoiserUseTitleLinearDepth.value_or_default())
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> taggedLinearDepth;
        RRTaggedResourceDiagnostic linearDepthDiagnostic {};

        // Titles that tag a resource without committing to a state declare COMMON, which
        // carries no shader-readable bit but is legal to transition from - the conversion
        // records the barrier out of the declared state and hands the resource back. The
        // transition allowance is a state policy on the acquisition itself, so this tag
        // clears the same frame/viewport/lifetime checks as every other signal: a legacy
        // tag whose Present/Evaluate lifetime may have expired is refused here like
        // anywhere else, no matter that the snapshot still holds the object alive.
        ID3D12Resource* linearDepthCandidate = nullptr;
        if (AcquireSLTaggedResource(rrTagSnapshot, RRTaggedSignal::LinearDepth, "Streamline.LinearDepth",
                                    TagStatePolicy::AllowCommonTransition, taggedLinearDepth, linearDepthDiagnostic))
        {
            linearDepthCandidate = taggedLinearDepth.Get();
        }

        if (linearDepthCandidate != nullptr)
        {
            const XMUINT2 base = linearDepthDiagnostic.usesExtent
                                     ? XMUINT2 { linearDepthDiagnostic.extentLeft, linearDepthDiagnostic.extentTop }
                                     : XMUINT2 {};
            const D3D12_RESOURCE_DESC linearDepthDesc = linearDepthCandidate->GetDesc();
            const DXGI_FORMAT linearDepthViewFormat = FSRD::GetViewFormat(linearDepthDesc.Format);

            if ((linearDepthViewFormat == DXGI_FORMAT_R32_FLOAT || linearDepthViewFormat == DXGI_FORMAT_R16_FLOAT) &&
                ValidateSourceExtent("TitleLinearDepth", linearDepthCandidate, base, renderWidth, renderHeight))
            {
                _titleLinearDepthTaggedResource = std::move(taggedLinearDepth);
                _convDesc.Resources.InTitleLinearDepth = _titleLinearDepthTaggedResource.Get();
                _convDesc.TitleLinearDepthBase = base;
                // The conversion records the barrier out of this state and back, so the state
                // has to travel with the resource rather than be assumed.
                _convDesc.TitleLinearDepthDeclaredState = linearDepthDiagnostic.state;
                _convDesc.TitleLinearDepthState = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;

                static bool loggedTitleLinearDepth = false;
                if (!loggedTitleLinearDepth)
                {
                    loggedTitleLinearDepth = true;
                    LOG_INFO("[RR_INPUT] title linear depth bound: {}x{}, base=({}, {}), {} "
                             "(declared state {:#x}); view-space positions and the denoiser's "
                             "depth input will use it",
                             linearDepthDesc.Width, linearDepthDesc.Height, base.x, base.y,
                             magic_enum::enum_name(linearDepthViewFormat), linearDepthDiagnostic.state);
                }
            }
            else
            {
                static bool loggedTitleLinearDepthRejection = false;
                if (!loggedTitleLinearDepthRejection)
                {
                    loggedTitleLinearDepthRejection = true;
                    LOG_WARN("[RR_INPUT] title linear depth present but unusable: {}x{} "
                             "(base ({}, {})), required {}x{}+({}, {})",
                             linearDepthDesc.Width, linearDepthDesc.Height, base.x, base.y, renderWidth, renderHeight,
                             base.x, base.y);
                }
            }
        }
        else
        {
            // A silent failure here is indistinguishable from the option never having been
            // enabled, which is exactly how a missing input is misread as a working one.
            static bool loggedTitleLinearDepthUnavailable = false;
            if (!loggedTitleLinearDepthUnavailable)
            {
                loggedTitleLinearDepthUnavailable = true;
                const size_t index = static_cast<size_t>(RRTaggedSignal::LinearDepth);
                if (index < rrTagSnapshot.resources.size())
                {
                    const RRTaggedResourceDiagnostic& diagnostic = rrTagSnapshot.resources[index].diagnostic;
                    LOG_WARN("[RR_INPUT] title linear depth is enabled but its tag could not be "
                             "acquired: observed={}, present={}, lifecycle={}, tagFrame={}, "
                             "tagViewport={}, activeFrame={}, activeViewport={}, "
                             "effectiveExtent={}x{}, declaredState={:#x}",
                             diagnostic.observed, diagnostic.present, magic_enum::enum_name(diagnostic.lifecycle),
                             diagnostic.frameIndex, diagnostic.viewport, rrTagSnapshot.activeEvaluationFrame,
                             rrTagSnapshot.activeEvaluationViewport, diagnostic.effectiveWidth,
                             diagnostic.effectiveHeight, diagnostic.state);
                }
                else
                {
                    LOG_WARN("[RR_INPUT] title linear depth is enabled but the tag snapshot has no "
                             "slot for it ({} slots)",
                             rrTagSnapshot.resources.size());
                }
            }
        }
    }

    // The title's responsivity hint. It is published under the title's own NGX key rather
    // than an NVSDK constant, and its polarity is the title's own convention, so only the
    // read is wired here and which side means "unstable" stays a setting.
    if (cfg.FfxDenoiserResponsivityThreshold.value_or_default() > 0.0f)
    {
        ID3D12Resource* responsivityMask = nullptr;
        TryGetNGXVoidPointer(inParams, "DLSSD.ResponsivityMask", responsivityMask);

        if (responsivityMask != nullptr &&
            ValidateReprojectionGuideSource("ResponsivityMask", responsivityMask, { 0u, 0u }, renderWidth, renderHeight,
                                            IsResponsivityMaskFormat,
                                            "R8_UNORM, R16_FLOAT, R32_FLOAT, RGBA8_UNORM or RGBA16_FLOAT"))
        {
            const D3D12_RESOURCE_DESC responsivityDesc = responsivityMask->GetDesc();
            _convDesc.Resources.InResponsivityMask = responsivityMask;

            // This runs on every Evaluate, so the log is only re-emitted when the bound
            // resource or the polarity/threshold configuration actually changes; otherwise
            // a long session accumulates an identical line per frame. The cache belongs to
            // this feature instance so independent instances cannot race or suppress one
            // another's diagnostics.
            const DXGI_FORMAT responsivityViewFormat = FSRD::GetViewFormat(responsivityDesc.Format);
            const float responsivityThreshold = cfg.FfxDenoiserResponsivityThreshold.value_or_default();
            const bool responsivityInvert = cfg.FfxDenoiserResponsivityInvert.value_or_default();

            if (responsivityMask != _loggedResponsivityMask || responsivityDesc.Width != _loggedResponsivityWidth ||
                responsivityDesc.Height != _loggedResponsivityHeight ||
                responsivityViewFormat != _loggedResponsivityViewFormat ||
                responsivityThreshold != _loggedResponsivityThreshold ||
                responsivityInvert != _loggedResponsivityInvert)
            {
                _loggedResponsivityMask = responsivityMask;
                _loggedResponsivityWidth = responsivityDesc.Width;
                _loggedResponsivityHeight = responsivityDesc.Height;
                _loggedResponsivityViewFormat = responsivityViewFormat;
                _loggedResponsivityThreshold = responsivityThreshold;
                _loggedResponsivityInvert = responsivityInvert;

                LOG_INFO("[RR_INPUT] responsivity hint bound: {}x{}, {}, threshold {:.4f} "
                         "({} counts as unstable)",
                         responsivityDesc.Width, responsivityDesc.Height, magic_enum::enum_name(responsivityViewFormat),
                         responsivityThreshold, responsivityInvert ? "above" : "below");
            }
        }
    }

    // Treat disappearance as a binding change, so the same resource is reported when it is
    // later published again instead of being suppressed by the last successful frame.
    if (_convDesc.Resources.InResponsivityMask == nullptr)
        _loggedResponsivityMask = nullptr;
}

// Binds the diffuse ray length, which RR's non-PSR handling of reflected geometry
// reads.
//
// Without it the diffuse signal declares every pixel a ray miss at infinity - the
// strongest possible claim, and almost always false when a title supplies one.
void FSRDFeatureDx12::ResolveDiffuseHitDistance(const NVSDK_NGX_Parameter& inParams, uint32_t renderWidth,
                                                uint32_t renderHeight)
{
    const auto& cfg = *Config::Instance();

    // Diagnostic-only probes for the two DLSS-RR diffuse hit-distance representations.
    // Do not bind or consume them until a title is confirmed to provide a compatible resource.
    _diffuseHitDistanceProbe = nullptr;
    _diffuseRayDirectionHitDistanceProbe = nullptr;
    _diffuseHitDistanceBaseX = 0;
    _diffuseHitDistanceBaseY = 0;
    _diffuseRayDirectionHitDistanceBaseX = 0;
    _diffuseRayDirectionHitDistanceBaseY = 0;
    TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSSD_DiffuseHitDistance, _diffuseHitDistanceProbe);
    TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirectionHitDistance,
                         _diffuseRayDirectionHitDistanceProbe);
    inParams.Get(NVSDK_NGX_Parameter_DLSSD_DiffuseHitDistance_Subrect_Base_X, &_diffuseHitDistanceBaseX);
    inParams.Get(NVSDK_NGX_Parameter_DLSSD_DiffuseHitDistance_Subrect_Base_Y, &_diffuseHitDistanceBaseY);
    inParams.Get(NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirectionHitDistance_Subrect_Base_X,
                 &_diffuseRayDirectionHitDistanceBaseX);
    inParams.Get(NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirectionHitDistance_Subrect_Base_Y,
                 &_diffuseRayDirectionHitDistanceBaseY);

    // Bind the diffuse ray length instead of only probing it. RR's non-PSR handling
    // of reflected geometry is driven by hit distance, and the diffuse signal has
    // been declaring every pixel a ray miss at infinity - the strongest possible
    // claim, and almost always false when the title actually supplies a length.
    _convDesc.Resources.InDiffuseHitDistance = nullptr;
    _convDesc.DiffuseHitDistanceBase = {};
    _convDesc.DiffuseHitDistanceMode = 0;

    if (cfg.FfxDenoiserDiffuseHitDistance.value_or_default())
    {
        const XMUINT2 scalarBase { _diffuseHitDistanceBaseX, _diffuseHitDistanceBaseY };
        const XMUINT2 combinedBase { _diffuseRayDirectionHitDistanceBaseX, _diffuseRayDirectionHitDistanceBaseY };

        if (_diffuseHitDistanceProbe &&
            ValidateReprojectionGuideSource(NVSDK_NGX_Parameter_DLSSD_DiffuseHitDistance, _diffuseHitDistanceProbe,
                                            scalarBase, renderWidth, renderHeight, IsDiffuseHitDistanceFormat,
                                            "R16F or R32F"))
        {
            _convDesc.Resources.InDiffuseHitDistance = _diffuseHitDistanceProbe;
            _convDesc.DiffuseHitDistanceBase = scalarBase;
            _convDesc.DiffuseHitDistanceMode = 1;
        }
        else if (_diffuseRayDirectionHitDistanceProbe &&
                 ValidateReprojectionGuideSource(NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirectionHitDistance,
                                                 _diffuseRayDirectionHitDistanceProbe, combinedBase, renderWidth,
                                                 renderHeight, IsDiffuseRayDirectionHitDistanceFormat,
                                                 "RGBA16F or RGBA32F (hit distance in alpha)"))
        {
            _convDesc.Resources.InDiffuseHitDistance = _diffuseRayDirectionHitDistanceProbe;
            _convDesc.DiffuseHitDistanceBase = combinedBase;
            _convDesc.DiffuseHitDistanceMode = 2;
        }
    }

    // InputBase4.zw was reserved; it now carries the diffuse hit-distance origin.
    _convDesc.InputBase4.z = _convDesc.DiffuseHitDistanceBase.x;
    _convDesc.InputBase4.w = _convDesc.DiffuseHitDistanceBase.y;
}

// Resolves the view and projection the denoiser reprojects with.
//
// A published matrix is only usable if it inverts to a real camera position. A
// partially filled or sentinel-filled one passes a presence check and poisons
// every reconstructed position with NaN, which reaches the denoiser as
// cameraPositionDelta and reads there as a camera that never moved.
bool FSRDFeatureDx12::ResolveCameraMatrices(const NVSDK_NGX_Parameter& inParams, const sl::Constants& slData,
                                            bool hasCurrentSLConstants)
{
    bool isReady = true;

    // Get DLSSD matrices and derive related values
    // World to view/camera space (V)
    _viewMatrix = {};
    _viewFromStreamline = false;

    // A matrix is only usable if it inverts to a real camera position, whichever
    // source produced it. A partially filled or all-zero one passes a presence
    // check, and the NaN it produces then reaches the denoiser as
    // cameraPositionDelta - a value it uses for its own reprojection. The motion
    // vectors mask the same NaN behind the shader's isfinite guard, where it reads
    // as a zero depth delta, so "the camera never moved" and "the camera position
    // was NaN" are indistinguishable from the outside.
    const auto viewMatrixIsUsable = [](const XMMATRIX& view, const XMMATRIX& invView)
    {
        if (!MatrixIsFinite(view) || !MatrixIsFinite(invView))
            return false;

        const float determinant = XMVectorGetX(XMMatrixDeterminant(view));
        if (!std::isfinite(determinant) || determinant == 0.0f)
            return false;

        const XMFLOAT3 cameraPosition = GetFloat3Column(invView, 3);
        return std::isfinite(cameraPosition.x) && std::isfinite(cameraPosition.y) && std::isfinite(cameraPosition.z);
    };

    // Builds the view matrix from the Streamline camera basis. Used both when NGX
    // publishes no matrix at all and when the published one cannot be inverted.
    // The basis is title data too: a degenerate or half-initialised one inverts to
    // the same poison a sentinel matrix would, so the result clears the same bar
    // as a published matrix before this fallback may claim success.
    const auto buildViewFromStreamline = [&]() -> bool
    {
        if (!StreamlineHooks::isSetConstantsHooked() || !hasCurrentSLConstants)
            return false;

        SetColumn(XMLoadFloat3((XMFLOAT3*) &slData.cameraRight), 0, _invViewMatrix);
        SetColumn(XMLoadFloat3((XMFLOAT3*) &slData.cameraUp), 1, _invViewMatrix);
        SetColumn(XMLoadFloat3((XMFLOAT3*) &slData.cameraFwd), 2, _invViewMatrix);
        SetColumn(XMLoadFloat3((XMFLOAT3*) &slData.cameraPos), 3, _invViewMatrix);
        _invViewMatrix.r[3].m128_f32[3] = 1.0f;

        _viewMatrix = XMMatrixInverse(nullptr, _invViewMatrix);

        if (!viewMatrixIsUsable(_viewMatrix, _invViewMatrix))
        {
            static bool loggedDegenerateSLView = false;
            if (!loggedDegenerateSLView)
            {
                loggedDegenerateSLView = true;
                LOG_ERROR("[RR_INPUT] the Streamline camera basis is degenerate; its view "
                          "matrix cannot be inverted, so camera position and reprojection "
                          "would read NaN. Rejecting the frame instead");
            }
            _viewMatrix = {};
            _invViewMatrix = {};
            return false;
        }

        _viewFromStreamline = true;
        return true;
    };

    XMMATRIX publishedViewMatrix = {};
    const bool hasPublishedViewMatrix =
        TryGetNGXColumnVectorMatrix(inParams, NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX, publishedViewMatrix);

    bool viewMatrixResolved = false;
    if (hasPublishedViewMatrix)
    {
        _viewMatrix = publishedViewMatrix;
        _invViewMatrix = XMMatrixInverse(nullptr, _viewMatrix);
        viewMatrixResolved = viewMatrixIsUsable(_viewMatrix, _invViewMatrix);

        if (!viewMatrixResolved)
        {
            static bool loggedDegenerateViewMatrix = false;
            if (!loggedDegenerateViewMatrix)
            {
                loggedDegenerateViewMatrix = true;
                const XMFLOAT3 cameraPosition = GetFloat3Column(_invViewMatrix, 3);
                LOG_ERROR("[RR_INPUT] the title's {} matrix cannot be inverted; camera position "
                          "reads ({}, {}, {}) and determinant is {}. Every consumer of the camera "
                          "position would receive NaN. Falling back to the Streamline camera "
                          "constants. Raw published matrix (row-major): "
                          "[{:.6f}, {:.6f}, {:.6f}, {:.6f}], [{:.6f}, {:.6f}, {:.6f}, {:.6f}], "
                          "[{:.6f}, {:.6f}, {:.6f}, {:.6f}], [{:.6f}, {:.6f}, {:.6f}, {:.6f}]",
                          NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX, cameraPosition.x, cameraPosition.y,
                          cameraPosition.z, XMVectorGetX(XMMatrixDeterminant(_viewMatrix)),
                          _viewMatrix.r[0].m128_f32[0], _viewMatrix.r[0].m128_f32[1], _viewMatrix.r[0].m128_f32[2],
                          _viewMatrix.r[0].m128_f32[3], _viewMatrix.r[1].m128_f32[0], _viewMatrix.r[1].m128_f32[1],
                          _viewMatrix.r[1].m128_f32[2], _viewMatrix.r[1].m128_f32[3], _viewMatrix.r[2].m128_f32[0],
                          _viewMatrix.r[2].m128_f32[1], _viewMatrix.r[2].m128_f32[2], _viewMatrix.r[2].m128_f32[3],
                          _viewMatrix.r[3].m128_f32[0], _viewMatrix.r[3].m128_f32[1], _viewMatrix.r[3].m128_f32[2],
                          _viewMatrix.r[3].m128_f32[3]);
            }
        }
    }

    if (!viewMatrixResolved && !buildViewFromStreamline())
    {
        LOG_ERROR("View matrix missing! Denoiser not ready.");
        _viewMatrix = {};
        _invViewMatrix = {};
        isReady = false;
    }

    if (_isInReset || !_hasDenoiserHistory)
        _prevViewMatrix = _viewMatrix;

    // Perspective projection matrix (P)
    _projMatrix = {};
    _projectionFromStreamline = false;

    // The projection is consumed through its inverse by the conversion shaders,
    // so it is only usable if that inverse exists and is finite, whichever source
    // produced it. A sentinel fill - one title publishes an all-FLT_MAX
    // world-to-view matrix - passes a presence check and then poisons every
    // reconstructed position with NaN.
    const auto projectionIsUsable = [&](const XMMATRIX& projection)
    {
        if (!MatrixIsFinite(projection))
            return false;

        const float determinant = XMVectorGetX(XMMatrixDeterminant(projection));
        if (!std::isfinite(determinant) || determinant == 0.0f)
            return false;

        return MatrixIsFinite(XMMatrixInverse(nullptr, projection));
    };

    // Reconstructs an unjittered projection from the Streamline scalar camera data,
    // which is what both the missing and the unusable published matrix fall back to.
    // The scalars are title data too: a zero aspect or a NaN FOV passes every
    // sentinel check and bakes straight into the matrix, so the rebuilt projection
    // must clear the same bar as a published one before this fallback may claim
    // success.
    const auto buildProjectionFromStreamline = [&]() -> bool
    {
        if (!StreamlineHooks::isSetConstantsHooked() || !hasCurrentSLConstants)
            return false;

        if (slData.cameraFOV == sl::INVALID_FLOAT || slData.cameraNear == sl::INVALID_FLOAT ||
            slData.cameraFar == sl::INVALID_FLOAT || slData.cameraAspectRatio == sl::INVALID_FLOAT ||
            slData.cameraNear == slData.cameraFar)
        {
            LOG_ERROR("Streamline projection data is incomplete! Denoiser not ready.");
            return false;
        }

        // These measurements are supposed to be in radians, but some titles supply degrees.
        // Valid FOV in radians never exceeds PI. Realistic FOV in degrees is basically never in the single
        // digits.
        const float fov = (slData.cameraFOV < 4.0f) ? slData.cameraFOV : GetRadiansFromDeg(slData.cameraFOV);
        const float nearPlane = slData.cameraNear;
        const float farPlane = slData.cameraFar;
        _isRightHanded = slData.cameraViewToClip[2].w < 0.0f;

        _projMatrix = CreateColumnVectorPerspectiveProjection(fov, slData.cameraAspectRatio, nearPlane, farPlane,
                                                              _isRightHanded, DepthInverted());

        if (!projectionIsUsable(_projMatrix))
        {
            static bool loggedDegenerateSLProjection = false;
            if (!loggedDegenerateSLProjection)
            {
                loggedDegenerateSLProjection = true;
                LOG_ERROR("[RR_INPUT] the Streamline projection scalars do not build an "
                          "invertible matrix (fov={:.4f}, aspect={:.4f}, near={:.4f}, "
                          "far={:.4f}); camera position and reprojection would read NaN. "
                          "Rejecting the frame instead",
                          fov, slData.cameraAspectRatio, nearPlane, farPlane);
            }
            _projMatrix = {};
            return false;
        }

        _projectionFromStreamline = true;
        return true;
    };

    XMMATRIX publishedProjMatrix = {};
    const bool hasPublishedProjMatrix =
        TryGetNGXColumnVectorMatrix(inParams, NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX, publishedProjMatrix);

    bool projMatrixResolved = false;
    if (hasPublishedProjMatrix)
    {
        _projMatrix = publishedProjMatrix;
        projMatrixResolved = projectionIsUsable(_projMatrix);

        if (!projMatrixResolved)
        {
            static bool loggedDegenerateProjMatrix = false;
            if (!loggedDegenerateProjMatrix)
            {
                loggedDegenerateProjMatrix = true;
                LOG_ERROR("[RR_INPUT] the title's {} matrix is not invertible, so no view-space "
                          "position can be reconstructed from it. Falling back to the Streamline "
                          "camera scalars. Raw published matrix (row-major): "
                          "[{:.6f}, {:.6f}, {:.6f}, {:.6f}], [{:.6f}, {:.6f}, {:.6f}, {:.6f}], "
                          "[{:.6f}, {:.6f}, {:.6f}, {:.6f}], [{:.6f}, {:.6f}, {:.6f}, {:.6f}]",
                          NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX, _projMatrix.r[0].m128_f32[0],
                          _projMatrix.r[0].m128_f32[1], _projMatrix.r[0].m128_f32[2], _projMatrix.r[0].m128_f32[3],
                          _projMatrix.r[1].m128_f32[0], _projMatrix.r[1].m128_f32[1], _projMatrix.r[1].m128_f32[2],
                          _projMatrix.r[1].m128_f32[3], _projMatrix.r[2].m128_f32[0], _projMatrix.r[2].m128_f32[1],
                          _projMatrix.r[2].m128_f32[2], _projMatrix.r[2].m128_f32[3], _projMatrix.r[3].m128_f32[0],
                          _projMatrix.r[3].m128_f32[1], _projMatrix.r[3].m128_f32[2], _projMatrix.r[3].m128_f32[3]);
            }
        }
    }

    if (!projMatrixResolved && !buildProjectionFromStreamline())
    {
        LOG_ERROR("Projection matrix missing! Denoiser not ready.");
        _projMatrix = {};
        isReady = false;
    }

    return isReady;
}

// Resolves the automatic diffuse and specular signal classification.
//
// Runs last, and only for a frame that cleared every validation before it: the
// lock is permanent, so a frame about to be rejected must not set it. The first
// Evaluate often arrives before the title has tagged its optional guides, and
// locking there would pin the classification for the life of the context.
bool FSRDFeatureDx12::ResolveSignalTypes(bool isReady, bool hasCurrentSLConstants)
{
    const auto& cfg = *Config::Instance();

    // AMD RR 1.2 requires a valid ray length in alpha for every active indirect-
    // specular pixel. The INI's unset value is Auto: resolve it once from a
    // semantically named, format/extent-validated guide, then keep the context
    // classification stable even if an optional tag temporarily disappears.
    //
    // This runs last, and only for a frame that cleared every validation above. The
    // lock is permanent, so a frame that is about to be rejected must not set it -
    // the first Evaluate frequently arrives before the title has tagged its optional
    // guides, and locking there would pin the classification to Direct for good.
    if (isReady && !cfg.FfxDenoiserSpecularSignalType.has_value() && !_autoSpecularSignalResolved)
    {
        // A guide is present when the specular step bound one - the same question, asked of
        // the frame's result rather than of the step's local selection.
        const bool hasSpecularHitDistanceGuide = _convDesc.Resources.InSpecHitDist != nullptr ||
                                                 _convDesc.Resources.InSpecularRayDirectionHitDistance != nullptr;

        const ffxStructType_t resolvedType = hasSpecularHitDistanceGuide
                                                 ? FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR
                                                 : FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR;

        LOG_INFO("[RR_INPUT] automatic specular classification resolved to {} on the first validated frame ({})",
                 GetSignalTypeName(resolvedType),
                 hasSpecularHitDistanceGuide ? "validated hit-distance guide present"
                                             : "no validated hit-distance guide; keeping every specular pixel active");

        if (_specularSignalDescType == resolvedType)
        {
            _autoSpecularSignalDescType = resolvedType;
            _autoSpecularSignalResolved = true;
        }
        else if (_preprocessorHasRecordedWork)
        {
            // Recreating in place here would destroy converter textures that an
            // already-submitted command list can still reference. UpdateSize refuses
            // exactly this for exactly this reason; take the same escape hatch and let
            // the rebuilt instance resolve on its own first frame, where nothing has
            // been recorded yet. The resolution is deliberately not latched, so the
            // fresh instance repeats it rather than inheriting a stale decision.
            LOG_INFO("[RR_INPUT] automatic specular classification needs {} -> {}; requesting a feature rebuild "
                     "because this instance has already recorded GPU work",
                     GetSignalTypeName(_specularSignalDescType), GetSignalTypeName(resolvedType));
            InvalidateDenoiserHistory();
            State::Instance().changeBackend[Handle()->Id] = true;
            return false;
        }
        else
        {
            LOG_INFO("[RR_INPUT] recreating RR context for automatic specular classification: {} -> {}",
                     GetSignalTypeName(_specularSignalDescType), GetSignalTypeName(resolvedType));
            _autoSpecularSignalDescType = resolvedType;
            _autoSpecularSignalResolved = true;
            DestroyDenoiserContext();
            if (!CreateDenoiserContext())
            {
                LOG_ERROR("[RR_INPUT] failed to recreate RR context for automatic specular classification; requesting "
                          "a feature rebuild");
                State::Instance().changeBackend[Handle()->Id] = true;
                return false;
            }
        }
    }

    // Same contract for the diffuse signal, resolved from the guide that signal
    // actually consumes: indirect diffuse reads a ray length from its alpha, direct
    // diffuse leaves that channel undefined. A title that supplies no diffuse ray
    // length therefore has nothing for the indirect path to work from, and every
    // pixel is better served staying active on the direct one.
    //
    // Deliberately a separate block rather than folded into the specular one above:
    // that block can return early to request a rebuild, and the two classifications
    // must not become order-dependent on each other.
    if (isReady && !cfg.FfxDenoiserDiffuseSignalType.has_value() && !_autoDiffuseSignalResolved)
    {
        const bool hasDiffuseRayLength =
            _convDesc.Resources.InDiffuseHitDistance != nullptr && _convDesc.DiffuseHitDistanceMode != 0u;

        const ffxStructType_t resolvedType = hasDiffuseRayLength ? FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE
                                                                 : FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE;

        LOG_INFO("[RR_INPUT] automatic diffuse classification resolved to {} on the first validated frame ({})",
                 GetSignalTypeName(resolvedType),
                 hasDiffuseRayLength ? "validated diffuse ray length present"
                                     : "no diffuse ray length; keeping every diffuse pixel active");

        if (_diffuseSignalDescType == resolvedType)
        {
            _autoDiffuseSignalDescType = resolvedType;
            _autoDiffuseSignalResolved = true;
        }
        else if (_preprocessorHasRecordedWork)
        {
            LOG_INFO("[RR_INPUT] automatic diffuse classification needs {} -> {}; requesting a feature rebuild because "
                     "this instance has already recorded GPU work",
                     GetSignalTypeName(_diffuseSignalDescType), GetSignalTypeName(resolvedType));
            InvalidateDenoiserHistory();
            State::Instance().changeBackend[Handle()->Id] = true;
            return false;
        }
        else
        {
            LOG_INFO("[RR_INPUT] recreating RR context for automatic diffuse classification: {} -> {}",
                     GetSignalTypeName(_diffuseSignalDescType), GetSignalTypeName(resolvedType));
            _autoDiffuseSignalDescType = resolvedType;
            _autoDiffuseSignalResolved = true;
            DestroyDenoiserContext();
            if (!CreateDenoiserContext())
            {
                LOG_ERROR("[RR_INPUT] failed to recreate RR context for automatic diffuse classification; requesting a "
                          "feature rebuild");
                State::Instance().changeBackend[Handle()->Id] = true;
                return false;
            }
        }
    }

    return isReady;
}

bool FSRDFeatureDx12::PrepareDenoiseConvInput(const NVSDK_NGX_Parameter& inParams)
{
    const auto& cfg = *Config::Instance();
    const SLConstantsSnapshot slConstantsSnapshot = StreamlineHooks::getSLConstantsSnapshot();
    const auto& slData = slConstantsSnapshot.constants;

    // Gather DLSS-RR input buffers for conversion and repacking for FSR-RR
    bool isReady = true;
    _convDesc.Resources = {};
    _convDesc.SpecularHitDistanceBase = {};
    _convDesc.SpecularHitDistanceFromCombinedAlpha = false;
    // The flag word is rebuilt from zero every frame, ahead of every producer
    // (ConvertDenoiserBuffers and ApplyDepthInterpretation, which only OR bits
    // in). Without this, a flag earned by an earlier frame's resources survives
    // that resource going away: a stale HasSpecHitDistance pins the scalar path
    // after the title moves to combined alpha, a stale TitleLinearDepth keeps
    // the shader reading a texture that is no longer bound, and a linear ->
    // hardware depth flip carries IsDepthLinear forward.
    _convDesc.Flags = 0;
    _specularHitDistanceTaggedResource.Reset();
    _specularRayDirectionHitDistanceTaggedResource.Reset();
    const RRD3D12SignalTagSnapshot rrTagSnapshot = StreamlineHooks::getRRD3D12SignalTagSnapshot();
    const bool hasCurrentSLConstants = rrTagSnapshot.activeEvaluationFrame != UINT32_MAX &&
                                       rrTagSnapshot.activeEvaluationViewport != UINT32_MAX &&
                                       slConstantsSnapshot.frameIndex == rrTagSnapshot.activeEvaluationFrame &&
                                       slConstantsSnapshot.viewport == rrTagSnapshot.activeEvaluationViewport;
    RRTaggedResourceDiagnostic emissiveTagDiagnostic {};

    // Standard TSR buffers
    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_Color, _convDesc.Resources.InColor))
        isReady = false;
    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_MotionVectors, _convDesc.Resources.InMotionVectors))
        isReady = false;
    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_Depth, _convDesc.Resources.InDepth))
        isReady = false;

    // DLSSD-specific buffers
    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_GBuffer_Normals, _convDesc.Resources.InNormals))
        isReady = false;

    // Roughness mode is instance state. When creation metadata is absent, lock it
    // once from the first usable frame; never flip modes because one resource is
    // temporarily missing, as that changes shader bindings and invalidates history.
    const bool hasSeparateRoughness =
        TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_GBuffer_Roughness, _convDesc.Resources.InRoughness);
    // A Streamline title tags NormalRoughness at the buffer where its normals and roughness really
    // live. Some titles leave the NGX GBuffer_Normals binding on a normals-only texture whose alpha
    // carries nothing -- RE Engine publishes R10G10B10A2 there, whose two alpha bits read back as
    // roughness 0 across the whole frame, and the denoiser then has no material guidance at all
    // while its own dispatch still reports success.
    //
    // Taken only when the NGX path published no separate roughness and the tagged buffer's alpha is
    // actually wide enough to hold it, so a title that supplies roughness properly keeps the binding
    // it declared.
    _normalRoughnessTaggedResource.Reset();

    if (!hasSeparateRoughness && cfg.FfxDenoiserTaggedNormalRoughness.value_or_default() &&
        !AlphaCarriesRoughness(_convDesc.Resources.InNormals))
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> taggedNormalRoughness;
        RRTaggedResourceDiagnostic normalRoughnessDiagnostic {};

        if (AcquireSLTaggedResource(rrTagSnapshot, RRTaggedSignal::NormalRoughness, "Streamline.NormalRoughness",
                                    TagStatePolicy::RequireShaderRead, taggedNormalRoughness,
                                    normalRoughnessDiagnostic))
        {
            const XMUINT2 base = normalRoughnessDiagnostic.usesExtent ? XMUINT2 { normalRoughnessDiagnostic.extentLeft,
                                                                                  normalRoughnessDiagnostic.extentTop }
                                                                      : XMUINT2 {};

            if (AlphaCarriesRoughness(taggedNormalRoughness.Get()) &&
                ValidateSourceExtent("Streamline.NormalRoughness", taggedNormalRoughness.Get(), base, RenderWidth(),
                                     RenderHeight()))
            {
                static bool loggedTaggedNormalRoughness = false;
                if (!loggedTaggedNormalRoughness)
                {
                    loggedTaggedNormalRoughness = true;
                    LOG_INFO("[RR_INPUT] using the Streamline NormalRoughness tag for normals and "
                             "packed roughness; the NGX normals binding ({}) has no alpha wide "
                             "enough to carry it",
                             (int) (_convDesc.Resources.InNormals ? _convDesc.Resources.InNormals->GetDesc().Format
                                                                  : DXGI_FORMAT_UNKNOWN));
                }

                _normalRoughnessTaggedResource = std::move(taggedNormalRoughness);
                _convDesc.Resources.InNormals = _normalRoughnessTaggedResource.Get();
            }
        }
    }

    if (_roughnessSource == RoughnessSource::Unknown)
    {
        if (hasSeparateRoughness)
        {
            _roughnessSource = RoughnessSource::Separate;
            LOG_INFO("DLSSD roughness metadata absent; locked this instance to the separate resource");
        }
        else if (SupportsPackedRoughness(_convDesc.Resources.InNormals))
        {
            _roughnessSource = RoughnessSource::Packed;
            LOG_INFO("DLSSD roughness metadata absent; locked this instance to normals alpha");

            // Worth saying out loud: a narrow alpha here is indistinguishable from a working bind
            // until the input probe is turned on and reads roughness 0 across the frame.
            if (!AlphaCarriesRoughness(_convDesc.Resources.InNormals))
                LOG_WARN("[RR_INPUT] the normals alpha this instance locked to is too narrow to "
                         "carry roughness; the denoiser will see roughness 0 everywhere unless the "
                         "title tags NormalRoughness through Streamline");
        }
        else
        {
            LOG_ERROR(
                "DLSSD roughness metadata and separate texture are absent, and normals format has no alpha channel");
            isReady = false;
        }
    }
    else if (_roughnessSource == RoughnessSource::Separate && !hasSeparateRoughness)
    {
        LOG_ERROR("DLSSD instance requires separate roughness, but the resource is missing this frame");
        InvalidateDenoiserHistory();
        isReady = false;
    }

    // Once per instance: what this instance actually bound for normals and roughness, and what the
    // Streamline tag path had to offer. Reading roughness 0 across the frame is indistinguishable
    // between "the source alpha is empty" and "the conversion dropped it" without this, and the
    // RR-facing probe reports the converted buffer's format rather than the title's.
    {
        static const void* loggedNormalsPtr = nullptr;

        if (loggedNormalsPtr != _convDesc.Resources.InNormals)
        {
            loggedNormalsPtr = _convDesc.Resources.InNormals;

            const auto normalsFormat =
                _convDesc.Resources.InNormals ? _convDesc.Resources.InNormals->GetDesc().Format : DXGI_FORMAT_UNKNOWN;

            Microsoft::WRL::ComPtr<ID3D12Resource> probeTagged;
            RRTaggedResourceDiagnostic probeDiagnostic {};
            const bool tagAcquired = AcquireSLTaggedResource(
                rrTagSnapshot, RRTaggedSignal::NormalRoughness, "Streamline.NormalRoughness.probe",
                TagStatePolicy::RequireShaderRead, probeTagged, probeDiagnostic);

            LOG_INFO("[RR_INPUT] normals bind: ptr={}, format={}, alphaCarriesRoughness={}, "
                     "separateRoughness={}, roughnessSource={} | SL NormalRoughness tag: "
                     "observed={}, present={}, acquired={}, format={}, effective={}x{}",
                     (const void*) _convDesc.Resources.InNormals, (int) normalsFormat,
                     AlphaCarriesRoughness(_convDesc.Resources.InNormals), hasSeparateRoughness,
                     _roughnessSource == RoughnessSource::Packed     ? "packed"
                     : _roughnessSource == RoughnessSource::Separate ? "separate"
                                                                     : "undetermined",
                     probeDiagnostic.observed, probeDiagnostic.present, tagAcquired,
                     (int) (probeTagged ? probeTagged->GetDesc().Format : DXGI_FORMAT_UNKNOWN),
                     probeDiagnostic.effectiveWidth, probeDiagnostic.effectiveHeight);
        }
    }

    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_DiffuseAlbedo, _convDesc.Resources.InDiffAlbedo))
        isReady = false;

    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_SpecularAlbedo, _convDesc.Resources.InSpecAlbedo))
        isReady = false;

    TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,
                         _convDesc.Resources.InBiasMask);

    // Optional diagnostic-only emissive input. Do not use it for RR signal
    // separation until its contents, exposure and composition semantics are verified.
    _emissiveProbe = nullptr;
    _emissiveTaggedResource.Reset();
    _emissiveProbeCompatible = false;
    _emissiveProbeFromStreamline = false;
    _convDesc.Resources.InEmissive = nullptr;
    TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_GBuffer_Emissive, _emissiveProbe);
    if (!_emissiveProbe)
    {
        _emissiveProbeFromStreamline =
            AcquireSLTaggedResource(rrTagSnapshot, RRTaggedSignal::Emissive, "Streamline.Emissive",
                                    TagStatePolicy::RequireShaderRead, _emissiveTaggedResource, emissiveTagDiagnostic);
        if (_emissiveProbeFromStreamline)
            _emissiveProbe = _emissiveTaggedResource.Get();
    }
    _emissiveProbeCompatible = IsEmissiveProbeCompatible(_emissiveProbe);
    if (_emissiveProbeCompatible)
        _convDesc.Resources.InEmissive = _emissiveProbe;

    // Optional legacy NGX GBuffer identifiers. NRD can use a material identifier
    // to reject history across material boundaries, but DLSS-RR does not require
    // either resource. Probe only; do not consume an unverified title-specific
    // encoding as temporal-history metadata.
    _materialIdProbe = nullptr;
    _shadingModelIdProbe = nullptr;
    TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_GBuffer_MaterialId, _materialIdProbe);
    TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_GBuffer_ShadingModelId, _shadingModelIdProbe);

    // Optional specular hit-distance inputs. Selection is deferred until all source
    // origins and extents are known, so an invalid high-priority candidate can never
    // reach an SRV binding.
    ID3D12Resource* ngxSpecularHitDistance = nullptr;
    ID3D12Resource* ngxSpecularRayDirectionHitDistance = nullptr;
    TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance, ngxSpecularHitDistance);
    TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance,
                         ngxSpecularRayDirectionHitDistance);

    const XMUINT2 colorBase = GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
                                             NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y);
    const XMUINT2 depthBase = GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                                             NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y);
    const XMUINT2 declaredMotionBase = GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                                                      NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y);
    XMUINT2 motionBase = declaredMotionBase;
    const XMUINT2 normalBase = GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_X,
                                              NVSDK_NGX_Parameter_DLSS_Input_Normals_Subrect_Base_Y);
    const XMUINT2 roughnessBase = GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_X,
                                                 NVSDK_NGX_Parameter_DLSS_Input_Roughness_Subrect_Base_Y);
    const XMUINT2 diffuseAlbedoBase =
        GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_X,
                       NVSDK_NGX_Parameter_DLSS_Input_DiffuseAlbedo_Subrect_Base_Y);
    const XMUINT2 specularAlbedoBase =
        GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_X,
                       NVSDK_NGX_Parameter_DLSS_Input_SpecularAlbedo_Subrect_Base_Y);
    const XMUINT2 ngxSpecularHitDistanceBase =
        GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_X,
                       NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance_Subrect_Base_Y);
    const XMUINT2 ngxSpecularRayDirectionHitDistanceBase =
        GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_X,
                       NVSDK_NGX_Parameter_DLSSD_SpecularRayDirectionHitDistance_Subrect_Base_Y);
    const XMUINT2 biasBase = GetSubrectBase(inParams, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X,
                                            NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y);

    XMUINT2 emissiveBase = colorBase;
    if (_emissiveProbeFromStreamline)
    {
        if (emissiveTagDiagnostic.present && emissiveTagDiagnostic.usesExtent)
            emissiveBase = { emissiveTagDiagnostic.extentLeft, emissiveTagDiagnostic.extentTop };
    }
    if (_convDesc.Resources.InEmissive &&
        !ValidateSourceExtent("Emissive", _convDesc.Resources.InEmissive, emissiveBase, RenderWidth(), RenderHeight()))
    {
        _convDesc.Resources.InEmissive = nullptr;
        _emissiveProbeCompatible = false;
    }

    _convDesc.FloorSourceBase = { colorBase.x, colorBase.y, depthBase.x, depthBase.y };
    _convDesc.InputBase1 = { normalBase.x, normalBase.y, roughnessBase.x, roughnessBase.y };
    _convDesc.InputBase2 = { ngxSpecularHitDistanceBase.x, ngxSpecularHitDistanceBase.y, diffuseAlbedoBase.x,
                             diffuseAlbedoBase.y };
    _convDesc.InputBase3 = { specularAlbedoBase.x, specularAlbedoBase.y, biasBase.x, biasBase.y };
    _convDesc.InputBase4 = { emissiveBase.x, emissiveBase.y, 0u, 0u };

    const uint32_t renderWidth = RenderWidth();
    const uint32_t renderHeight = RenderHeight();
    uint32_t motionWidth = LowResMV() ? RenderWidth() : DisplayWidth();
    uint32_t motionHeight = LowResMV() ? RenderHeight() : DisplayHeight();
    bool displayResolutionMotion = !LowResMV();

    // Streamline may replace a title's display-resolution MV with its own
    // camera-completed render-resolution texture before forwarding the NGX call.
    // Feature-creation metadata still describes the original resource, so the
    // actual D3D12 extent must win when the two contracts no longer fit.
    if (_convDesc.Resources.InMotionVectors)
    {
        const D3D12_RESOURCE_DESC motionDesc = _convDesc.Resources.InMotionVectors->GetDesc();
        const bool declaredExtentFits = uint64_t(motionBase.x) + motionWidth <= motionDesc.Width &&
                                        uint64_t(motionBase.y) + motionHeight <= motionDesc.Height;
        const bool zeroBasedLogicalExtentFits =
            uint64_t(motionWidth) <= motionDesc.Width && uint64_t(motionHeight) <= motionDesc.Height;
        const bool zeroBasedRenderExtentFits =
            uint64_t(RenderWidth()) <= motionDesc.Width && uint64_t(RenderHeight()) <= motionDesc.Height;
        if (!declaredExtentFits && zeroBasedLogicalExtentFits)
        {
            LOG_WARN("[RR_INPUT] NGX motion resource is {}x{} and cannot contain the declared "
                     "subrect {}x{}+({},{}); retaining its logical resolution with a "
                     "zero-based origin",
                     motionDesc.Width, motionDesc.Height, motionWidth, motionHeight, motionBase.x, motionBase.y);
            motionBase = { 0u, 0u };
        }
        else if (!declaredExtentFits && displayResolutionMotion && zeroBasedRenderExtentFits)
        {
            LOG_WARN("[RR_INPUT] NGX motion resource is {}x{} and cannot contain the declared "
                     "display-resolution subrect {}x{}+({},{}); treating it as Streamline's "
                     "zero-based render-resolution camera-completed motion",
                     motionDesc.Width, motionDesc.Height, motionWidth, motionHeight, motionBase.x, motionBase.y);
            motionWidth = RenderWidth();
            motionHeight = RenderHeight();
            motionBase = { 0u, 0u };
            displayResolutionMotion = false;
        }
    }

    _convDesc.InputBase0 = { colorBase.x, colorBase.y, motionBase.x, motionBase.y };
    if (motionWidth == 0 || motionHeight == 0)
    {
        LOG_ERROR("[RR_INPUT] motion-vector extent is zero");
        isReady = false;
    }
    else
    {
        float motionScaleX = 1.0f;
        float motionScaleY = 1.0f;
        const bool validMotionScaleX =
            inParams.Get(NVSDK_NGX_Parameter_MV_Scale_X, &motionScaleX) == NVSDK_NGX_Result_Success &&
            std::isfinite(motionScaleX) && motionScaleX != 0.0f;
        const bool validMotionScaleY =
            inParams.Get(NVSDK_NGX_Parameter_MV_Scale_Y, &motionScaleY) == NVSDK_NGX_Result_Success &&
            std::isfinite(motionScaleY) && motionScaleY != 0.0f;
        if (!validMotionScaleX)
            motionScaleX = 1.0f;
        if (!validMotionScaleY)
            motionScaleY = 1.0f;
        if (!validMotionScaleX || !validMotionScaleY)
        {
            // NGX helper APIs normalize a missing, non-finite, or zero component
            // to one. Preserve a valid negative component because it can encode
            // the title's axis convention.
            LOG_WARN("[RR_INPUT] MV scale component missing/invalid/zero (x={}, y={}); "
                     "using NGX's unit fallback per component",
                     validMotionScaleX, validMotionScaleY);
        }

        _convDesc.MotionInputSize = { static_cast<float>(motionWidth), static_cast<float>(motionHeight),
                                      1.0f / static_cast<float>(motionWidth), 1.0f / static_cast<float>(motionHeight) };
        _convDesc.MotionTransform = { // NGX MV scale converts the stored value to render-pixel motion.
                                      // The texture extent only controls where that value is fetched; UV
                                      // normalization always uses the render extent, including high-res MV.
                                      motionScaleX / static_cast<float>(RenderWidth()),
                                      motionScaleY / static_cast<float>(RenderHeight()), 0.0f, 0.0f
        };
    }

    ResolveSpecularHitDistance(inParams, rrTagSnapshot, renderWidth, renderHeight, motionWidth, motionHeight,
                               ngxSpecularHitDistance, ngxSpecularRayDirectionHitDistance, ngxSpecularHitDistanceBase,
                               ngxSpecularRayDirectionHitDistanceBase);

    AcquireOptionalInputs(inParams, rrTagSnapshot, renderWidth, renderHeight);

    float jitterX = 0.0f;
    float jitterY = 0.0f;
    inParams.Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
    inParams.Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);
    if (!std::isfinite(jitterX))
        jitterX = 0.0f;
    if (!std::isfinite(jitterY))
        jitterY = 0.0f;

    const XMFLOAT2 currentJitter { jitterX, jitterY };
    const XMFLOAT2 previousJitter = (_isInReset || !_hasDenoiserHistory) ? currentJitter : _previousDenoiserJitter;
    _convDesc.JitterOffsets = { currentJitter.x, currentJitter.y, previousJitter.x, previousJitter.y };
    _convDesc.MotionHistoryValid = _hasDenoiserHistory && !_isInReset;
    _convDesc.DisplayResolutionMotion = displayResolutionMotion;
    _convDesc.MotionVectorsJittered = JitteredMV();

    isReady &= ValidateSourceExtent("Color", _convDesc.Resources.InColor, colorBase, renderWidth, renderHeight);
    isReady &= ValidateSourceExtent("Depth", _convDesc.Resources.InDepth, depthBase, renderWidth, renderHeight);
    isReady &= ValidateSourceExtent("MotionVectors", _convDesc.Resources.InMotionVectors, motionBase, motionWidth,
                                    motionHeight);
    isReady &= ValidateSourceExtent("Normals", _convDesc.Resources.InNormals, normalBase, renderWidth, renderHeight);
    if (_roughnessSource == RoughnessSource::Separate)
        isReady &= ValidateSourceExtent("Roughness", _convDesc.Resources.InRoughness, roughnessBase, renderWidth,
                                        renderHeight);
    isReady &= ValidateSourceExtent("DiffuseAlbedo", _convDesc.Resources.InDiffAlbedo, diffuseAlbedoBase, renderWidth,
                                    renderHeight);
    isReady &= ValidateSourceExtent("SpecularAlbedo", _convDesc.Resources.InSpecAlbedo, specularAlbedoBase, renderWidth,
                                    renderHeight);
    if (_convDesc.Resources.InBiasMask)
        isReady &= ValidateSourceExtent("BiasCurrentColor", _convDesc.Resources.InBiasMask, biasBase, renderWidth,
                                        renderHeight);

    ResolveDiffuseHitDistance(inParams, renderWidth, renderHeight);

    isReady &= ResolveCameraMatrices(inParams, slData, hasCurrentSLConstants);

    if (!ResolveSignalTypes(isReady, hasCurrentSLConstants))
        return false;

    return isReady;
}

// Decides whether the title's depth is hardware or already linear, and applies it.
//
// The precedence is user override, then conclusive evidence from the resource itself, then the
// title's NGX declaration, then a base-rate assumption - and the declaration deliberately does
// not outrank a depth-stencil resource. NGX publishes DLSS.Use.HW.Depth from
// NVSDK_NGX_DLSSD_Create_Params::depthType, where Linear is zero, so every title that
// zero-initializes that struct "declares" linear without meaning to. A declaration is therefore
// only trustworthy when the resource does not contradict it, and nothing writes a linearised
// view-space distance into a depth-stencil attachment.
//
// Applied per frame rather than latched at init so it can be toggled while watching the depth
// debug view. Switching changes the units of the internal previous-depth texture, so it resets
// temporal history.
// Decides whether the title's depth is hardware or already linear, and applies it.
//
// The precedence is user override, then conclusive evidence from the resource itself, then the
// title's NGX declaration, then a base-rate assumption - and the declaration deliberately does
// not outrank a depth-stencil resource. NGX publishes DLSS.Use.HW.Depth from
// NVSDK_NGX_DLSSD_Create_Params::depthType, where Linear is zero, so every title that
// zero-initializes that struct "declares" linear without meaning to. A declaration is therefore
// only trustworthy when the resource does not contradict it, and nothing writes a linearised
// view-space distance into a depth-stencil attachment.
//
// Applied per frame rather than latched at init so it can be toggled while watching the depth
// debug view. Switching changes the units of the internal previous-depth texture, so it resets
// temporal history.
void FSRDFeatureDx12::ApplyDepthInterpretation()
{
    const auto& cfg = *Config::Instance();

    // Resolve the effective depth interpretation. A user override wins over whatever
    // NGX reported, and it is applied per frame rather than latched at init so it can
    // be toggled while watching the depth debug view. Switching changes the units of
    // the internal previous-depth texture, so it has to reset temporal history.
    //
    // Precedence: user override, then conclusive evidence from the resource itself,
    // then the NGX declaration, then a base-rate assumption.
    //
    // The declaration deliberately does NOT outrank a depth-stencil resource. NGX
    // publishes DLSS.Use.HW.Depth from NVSDK_NGX_DLSSD_Create_Params::depthType, and
    // Linear is zero - so every title that zero-initializes that struct "declares"
    // linear without meaning to. A declaration is therefore only trustworthy when the
    // resource does not contradict it, and nothing writes a linearized view-space
    // distance into a depth-stencil attachment.
    const DepthResourceKind depthKind = ClassifyDepthResource(_convDesc.Resources.InDepth);

    bool hardwareDepth;
    const char* depthSource;

    if (cfg.FfxDenoiserHardwareDepth.has_value())
    {
        hardwareDepth = cfg.FfxDenoiserHardwareDepth.value();
        depthSource = "user override";
    }
    else if (depthKind == DepthResourceKind::HardwareDepth)
    {
        hardwareDepth = true;
        depthSource = (_hasNGXDepthType && !_ngxReportedHWDepth)
                          ? "depth-stencil resource, overriding the title's linear declaration"
                          : "depth-stencil resource";
    }
    else if (_hasNGXDepthType)
    {
        hardwareDepth = _ngxReportedHWDepth;
        depthSource = "NGX declaration";
    }
    else if (depthKind == DepthResourceKind::GameWritten)
    {
        // Render-target or UAV capable and not depth-stencil: the title's own shaders
        // filled this, which is what a pre-linearized depth target looks like.
        hardwareDepth = false;
        depthSource = "undeclared; game-written target, assuming linear";
    }
    else
    {
        // Undeclared and unclassifiable. Hardware depth is the overwhelmingly common
        // case in DLSS-RR titles, and guessing linear here is the failure that
        // collapses the whole scene into [near, 1].
        hardwareDepth = true;
        depthSource = "undeclared and unclassifiable, assuming hardware";
    }

    const int appliedHardwareDepth = hardwareDepth ? 1 : 0;

    if (_appliedHardwareDepth != appliedHardwareDepth)
    {
        if (_appliedHardwareDepth >= 0)
        {
            LOG_INFO("[RR_INPUT] depth interpretation changed: {} -> {}; resetting denoiser history",
                     _appliedHardwareDepth != 0 ? "hardware" : "linear", hardwareDepth ? "hardware" : "linear");
            InvalidateDenoiserHistory();
        }
        else
        {
            // Report the evidence, not just the verdict: when the resource is
            // unclassifiable this is the only way to tell why auto chose what it did.
            DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
            uint32_t depthFlags = 0;
            if (_convDesc.Resources.InDepth)
            {
                const D3D12_RESOURCE_DESC depthDesc = _convDesc.Resources.InDepth->GetDesc();
                depthFormat = depthDesc.Format;
                depthFlags = static_cast<uint32_t>(depthDesc.Flags);
            }

            LOG_INFO("[RR_INPUT] depth interpretation: {} (source: {}); resource format={}, "
                     "resourceFlags={:#x}, ngxDeclared={}",
                     hardwareDepth ? "hardware" : "linear", depthSource, magic_enum::enum_name(depthFormat), depthFlags,
                     _hasNGXDepthType ? (_ngxReportedHWDepth ? "hardware" : "linear") : "absent");
        }

        _appliedHardwareDepth = appliedHardwareDepth;
    }

    _isHWDepth = hardwareDepth;

    if (!_isHWDepth)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::IsDepthLinear;
}

bool FSRDFeatureDx12::ConvertDenoiserBuffers(ID3D12GraphicsCommandList* InCommandList)
{
    const auto& cfg = *Config::Instance();
    const auto dbgMode = static_cast<DebugModes>(cfg.FfxDenoiserDebugMode.value_or_default());
    // Prepare input converter
    _convDesc.RenderSize = { (float) RenderWidth(), (float) RenderHeight(), 1.0f / (float) RenderWidth(),
                             1.0f / (float) RenderHeight() };

    if (_convDesc.MotionVectorsJittered)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::MotionVectorsJittered;
    if (_convDesc.DisplayResolutionMotion)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::DisplayResolutionMotion;
    // The packing shader selects its debug view from the flag word's debug bits
    // (GetDebugMode) and writes it to the specular signal output, which is what
    // the debug blit shows. Nothing else carried the user's selection there, so
    // every conversion debug mode rendered the packed signal instead. Non-
    // conversion modes mask to zero here, leaving the pass untouched.
    _convDesc.Flags |= (uint32_t) GetConvDebugFlags(dbgMode);
    const bool normalsInViewSpace = cfg.FfxDenoiserNormalsInViewSpace.value_or_default();
    if (normalsInViewSpace)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::NormalsViewSpace;

    const int appliedNormalsInViewSpace = normalsInViewSpace ? 1 : 0;
    if (_appliedNormalsInViewSpace != appliedNormalsInViewSpace)
    {
        if (_appliedNormalsInViewSpace >= 0)
        {
            LOG_INFO("[RR_INPUT] normal space changed: {}->{}; resetting denoiser history",
                     _appliedNormalsInViewSpace != 0 ? "view" : "world", normalsInViewSpace ? "view" : "world");
            InvalidateDenoiserHistory();
        }
        _appliedNormalsInViewSpace = appliedNormalsInViewSpace;
    }
    if (_specularSignalDescType == FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::SpecularSignalIndirect;
    _convDesc.FloorIsolation = std::clamp(cfg.FfxDenoiserFloorIsolation.value_or_default(), 0.0f, 1.0f);
    _convDesc.RoughnessFloor = std::clamp(cfg.FfxDenoiserRoughnessFloor.value_or_default(), 0.0f, 1.0f);
    _convDesc.FloorHandoverMode =
        static_cast<uint32_t>(std::clamp(cfg.FfxDenoiserFloorHandover.value_or_default(), 0, 2));
    _convDesc.FloorHandoverStrength = std::clamp(cfg.FfxDenoiserFloorHandoverStrength.value_or_default(), 0.0f, 1.0f);
    _convDesc.FloorRawBlend = std::clamp(cfg.FfxDenoiserFloorRawBlend.value_or_default(), 0.0f, 1.0f);
    _convDesc.FloorStructureGate = std::clamp(cfg.FfxDenoiserFloorStructureGate.value_or_default(), 0.0f, 1.0f);
    _convDesc.DemodDivisorFloor = std::clamp(cfg.FfxDenoiserDemodDivisorFloor.value_or_default(), 1e-4f, 0.5f);
    _convDesc.FloorClampSmoothing = std::clamp(cfg.FfxDenoiserFloorClampSmoothing.value_or_default(), 0.0f, 1.0f);
    _convDesc.FloorHandoverDetail = std::clamp(cfg.FfxDenoiserFloorHandoverDetail.value_or_default(), 0.0f, 1.0f);

    _convDesc.Resources.InInspector = nullptr;
    _convDesc.InspectorChannel = ResTrack_Dx12::GetRRResourceChannel();
    _convDesc.InspectorScale = ResTrack_Dx12::GetRRResourceViewScale();
    // Same clamp as the RR debug-bounds configure key below and as the menu slider.
    // They previously disagreed - max(v, 1.0) here against clamp(v, 0.001, 1024)
    // there - so any value under 1.0 normalized OptiScaler's own NormDepth view
    // differently from AMD's, and the two could not be compared.
    _convDesc.DebugDepthMax = std::clamp(cfg.FfxDenoiserDebugDepthMax.value_or_default(), 0.001f, 1024.0f);

    Microsoft::WRL::ComPtr<ID3D12Resource> inspectorResource;
    if (ResTrack_Dx12::IsRRResourceInspectorEnabled())
    {
        ResTrack_Dx12::NotifyRRResourceInspectorFrame(_frameCount);
        ResTrack_Dx12::RefreshRRResourceCandidates(RenderWidth(), RenderHeight());
        if (dbgMode == DebugModes::ResourceInspector)
        {
            inspectorResource.Attach(ResTrack_Dx12::AcquireRRResourceCandidate());
            _convDesc.Resources.InInspector = inspectorResource.Get();
        }
    }

    if (_convDesc.RoughnessFloor != _appliedRoughnessFloor)
    {
        if (_appliedRoughnessFloor >= 0.0f)
        {
            LOG_INFO("[RR_DIAG] RR roughness floor changed: floor {:.4f}->{:.4f}; "
                     "resetting denoiser history",
                     _appliedRoughnessFloor, _convDesc.RoughnessFloor);
        }
        else
        {
            LOG_INFO("[RR_DIAG] RR roughness floor initialized: floor={:.4f}", _convDesc.RoughnessFloor);
        }

        _appliedRoughnessFloor = _convDesc.RoughnessFloor;
        InvalidateDenoiserHistory();
    }

    if (_roughnessSource == RoughnessSource::Packed)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::IsRoughnessPacked;

    if (_convDesc.Resources.InSpecHitDist)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::HasSpecHitDistance;
    if (_convDesc.Resources.InEmissive)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::HasEmissiveInput;

    // Flags follow the resources that were actually validated this frame, so an unavailable
    // or rejected input leaves the shader on its derived path instead of reading a texture
    // that was never bound.
    if (_convDesc.Resources.InTitleLinearDepth != nullptr)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::TitleLinearDepth;
    if (_convDesc.Resources.InResponsivityMask != nullptr)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::HasResponsivityMask;

    _convDesc.ResponsivityTrustThreshold = std::max(cfg.FfxDenoiserResponsivityThreshold.value_or_default(), 0.0f);
    _convDesc.ResponsivityInvert = cfg.FfxDenoiserResponsivityInvert.value_or_default();
    _convDesc.DiagnosticsEnabled = cfg.FfxDenoiserDiagnostics.value_or_default();

    _convDesc.BiasMaskStrength = cfg.FfxDenoiserBiasMaskStrength.value_or_default();
    _convDesc.FloorDetailBoost = cfg.FfxDenoiserFloorDetailBoost.value_or_default();
    _convDesc.FloorNormalSharpness = cfg.FfxDenoiserFloorNormalSharpness.value_or_default();
    _convDesc.FloorAlbedoGuide = cfg.FfxDenoiserFloorAlbedoGuide.value_or_default();
    _convDesc.FloorLumSymmetry = cfg.FfxDenoiserFloorLumSymmetry.value_or_default();
    _convDesc.FloorGrazingSharpness = cfg.FfxDenoiserFloorGrazingSharpness.value_or_default();
    _convDesc.FloorEnvelopeBias = cfg.FfxDenoiserFloorEnvelopeBias.value_or_default();
    _convDesc.FloorSoftMin = cfg.FfxDenoiserFloorSoftMin.value_or_default();

    StoreHlslColumnVectorMatrix(_convDesc.InvViewMatrix, _invViewMatrix);

    // Inverse perspective projection
    const XMMATRIX invProjMatrix = XMMatrixInverse(nullptr, _projMatrix);
    StoreHlslColumnVectorMatrix(_convDesc.InvProjMatrix, invProjMatrix);

    // Previous world to view for linear depth delta
    StoreHlslColumnVectorMatrix(_convDesc.PrevViewMatrix, _prevViewMatrix);

    // Near and far planes
    const ViewPlanes planes = GetViewPlanes(_projMatrix, DepthInverted());
    _convDesc.NearPlane = planes.nearPlane;
    _convDesc.FarPlane = planes.farPlane;
    _isRightHanded = planes.isRightHanded;

    // Every geometric quantity downstream - linearised depth, world-position
    // reconstruction and the far-plane skip test - is derived from these two numbers
    // and the raw projection terms behind them. Report them directly rather than inferring
    // them from debug-view colours, which cannot distinguish a depth clamped to
    // near from one clamped to far. Logged only when the values change.
    {
        static float loggedNear = -1.0f;
        static float loggedFar = -1.0f;
        if (planes.nearPlane != loggedNear || planes.farPlane != loggedFar)
        {
            loggedNear = planes.nearPlane;
            loggedFar = planes.farPlane;
            LOG_INFO("[RR_DIAG] view planes: near={}, far={}, infinite={}, rightHanded={}, "
                     "depthInverted={}, hardwareDepth={}, projectionFromStreamline={}, "
                     "clipA={}, clipB={}, clipW={}",
                     planes.nearPlane, planes.farPlane, planes.isInfinite, planes.isRightHanded, DepthInverted(),
                     _isHWDepth, _projectionFromStreamline, _projMatrix.r[2].m128_f32[2], _projMatrix.r[2].m128_f32[3],
                     _projMatrix.r[3].m128_f32[2]);
        }
    }

    if (_isRightHanded)
        _convDesc.Flags |= (uint32_t) FSRDConvFlags::RightHanded;

    ApplyDepthInterpretation();

    // What this frame's view-space positions are actually built from. It has to be read here,
    // after ApplyDepthInterpretation and the right-handed flag, because both are part of the
    // definition - and it has to be the *effective* outcome of AcquireOptionalInputs rather
    // than the option that drives it. Enabling the option on a title that publishes no tagged
    // field leaves the definition untouched, and a title's tag becoming usable or unusable
    // changes it with no menu interaction at all, which is the half of this that a config
    // comparison cannot see.
    const DepthDefinition depthDefinition = { .titleLinearDepth = _convDesc.Resources.InTitleLinearDepth != nullptr,
                                              .rightHanded = _isRightHanded };

    if (_hasAppliedDepthDefinition && depthDefinition != _appliedDepthDefinition)
    {
        LOG_INFO("[RR_INPUT] depth definition changed: {} (right-handed {}) -> {} "
                 "(right-handed {}); resetting denoiser history",
                 _appliedDepthDefinition.titleLinearDepth ? "title linear depth" : "derived depth",
                 _appliedDepthDefinition.rightHanded,
                 depthDefinition.titleLinearDepth ? "title linear depth" : "derived depth",
                 depthDefinition.rightHanded);
        InvalidateDenoiserHistory();
    }
    _appliedDepthDefinition = depthDefinition;
    _hasAppliedDepthDefinition = true;

    // Every check that resets history - the normal space and roughness floor above, the depth
    // interpretation just above this, and the depth definition in between - runs after
    // PrepareDenoiseConvInput already froze the reprojection inputs from the value
    // _hasDenoiserHistory had at the time. Re-derive them once here, after the last of those
    // producers and before the conversion dispatch that consumes them, or the conversion pass
    // reprojects against a previous-frame depth produced under the setting that was just
    // abandoned on the very frame the RR dispatch resets. The denoiser dispatch that follows
    // reads _hasDenoiserHistory for its own reset flag, so it sees the same decision.
    RefreshHistoryDerivedInputs();

    // Per-frame lines stay behind the switch: they say nothing a shipped build needs, and a
    // session of them is a large part of a gigabyte of log.
    if (_convDesc.DiagnosticsEnabled)
        LOG_DEBUG("Dispatching FSRD Input Converter");

    // Dispatch resource converter. Outputs are automatically transitioned for reading.
    if (!FSRDConvShader->DispatchConversion(InCommandList, _convDesc))
    {
        _convDesc.Resources.InInspector = nullptr;
        return false;
    }

    // From here on the converter's textures are referenced by a command list the
    // application will submit, so they must not be released out from under it.
    _preprocessorHasRecordedWork = true;

    _convDesc.Resources.InInspector = nullptr;
    return true;
}

static bool ValidateRRDispatchChain(ID3D12GraphicsCommandList* commandList, const ffxDispatchDescDenoiser& dispatchDesc,
                                    ffxStructType_t expectedDiffuseType, ffxStructType_t expectedSpecularType,
                                    bool expectDiffuseSignal, bool expectSpecularSignal, bool expectedAmbientOcclusion)
{
    if (!commandList || dispatchDesc.commandList != commandList)
    {
        LOG_ERROR("RR 1.2 dispatch has a null or mismatched D3D12 command list");
        return false;
    }

    if (dispatchDesc.header.type != FFX_API_DISPATCH_DESC_TYPE_DENOISER)
    {
        LOG_ERROR("RR 1.2 dispatch head has an invalid descriptor type: {0:X}", dispatchDesc.header.type);
        return false;
    }

    bool foundDiffuse = false;
    bool foundSpecular = false;
    bool foundAmbientOcclusion = false;
    bool foundDebugView = false;

    for (const ffxDispatchDescHeader* signal = dispatchDesc.header.pNext; signal != nullptr; signal = signal->pNext)
    {
        if (signal->type == FFX_API_DISPATCH_DESC_TYPE_DENOISER_DEBUG_VIEW)
        {
            if (foundDebugView || signal->pNext)
            {
                LOG_ERROR("RR 1.2 debug descriptor must appear once at the chain tail");
                return false;
            }

            foundDebugView = true;
            continue;
        }

        bool* found = nullptr;
        if (signal->type == expectedDiffuseType)
            found = &foundDiffuse;
        else if (signal->type == expectedSpecularType)
            found = &foundSpecular;
        else if (signal->type == FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION)
            found = &foundAmbientOcclusion;
        else
        {
            LOG_ERROR("RR 1.2 dispatch contains unexpected descriptor {} ({:#x})", GetSignalTypeName(signal->type),
                      signal->type);
            return false;
        }

        if (*found)
        {
            LOG_ERROR("RR 1.2 dispatch contains duplicate {} descriptor", GetSignalTypeName(signal->type));
            return false;
        }
        *found = true;
    }

    if (foundDiffuse != expectDiffuseSignal || foundSpecular != expectSpecularSignal ||
        foundAmbientOcclusion != expectedAmbientOcclusion)
    {
        LOG_ERROR("RR 1.2 dispatch signal mismatch: diffuse={}, specular={}, AO={}; "
                  "expected diffuse={}, specular={}, AO={}",
                  foundDiffuse, foundSpecular, foundAmbientOcclusion, expectDiffuseSignal, expectSpecularSignal,
                  expectedAmbientOcclusion);
        return false;
    }

    return true;
}

bool FSRDFeatureDx12::DispatchDenoiser(ID3D12GraphicsCommandList* InCommandList,
                                       const ffxDispatchDescDenoiser& dispatchDesc)
{
    auto& state = State::Instance();
    const auto& cfg = *Config::Instance();

    if (!_pDenoiserCtx)
    {
        LOG_ERROR("RR 1.2 dispatch attempted without a denoiser context");
        return false;
    }

    if (!ValidateRRDispatchChain(InCommandList, dispatchDesc, _diffuseSignalDescType, _specularSignalDescType,
                                 _denoiseDiffuse, _denoiseSpecular, _ambientOcclusionEnabled))
        return false;

    const ffxDispatchDescHeader* diffuseHeader = nullptr;
    const ffxDispatchDescHeader* specularHeader = nullptr;
    const ffxDispatchDescHeader* ambientOcclusionHeader = nullptr;
    for (const ffxDispatchDescHeader* signal = dispatchDesc.header.pNext; signal != nullptr; signal = signal->pNext)
    {
        if (signal->type == _diffuseSignalDescType)
            diffuseHeader = signal;
        else if (signal->type == _specularSignalDescType)
            specularHeader = signal;
        else if (signal->type == FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION)
            ambientOcclusionHeader = signal;
    }

    // All four RR 1.2 diffuse/specular descriptor structures have the same
    // header + signal ABI. The headers stay nullable: a single-signal chain
    // legitimately omits one, so references are formed only where that
    // signal's presence is known.
    const auto* ambientOcclusion =
        ambientOcclusionHeader
            ? reinterpret_cast<const ffxDispatchDescDenoiserAmbientOcclusion*>(ambientOcclusionHeader)
            : nullptr;
    const bool resetRequested = !!(dispatchDesc.flags & FFX_DENOISER_DISPATCH_RESET);
    const bool resetTransition = resetRequested && !_lastDispatchRequestedReset;
    const bool logDispatchSnapshot = _logNextDenoiserDispatch || resetTransition;

    if (logDispatchSnapshot)
    {
        // Single-signal mode: LogRRDispatchSnapshot's resource inventory and the
        // per-signal probe battery both assume the two-signal chain; something in
        // there throws under the SL VEH when the diffuse descriptor is absent
        // (hundreds of swallowed dumps per session, dispatch never reached). The
        // chain shape is the information that matters, so log it directly here.
        if (_denoiseDiffuse && _denoiseSpecular)
        {
            // The validated chain carries both signals here, so the header
            // dereference below is sound.
            const auto& directDiffuse = *reinterpret_cast<const ffxDispatchDescDenoiserDirectDiffuse*>(diffuseHeader);
            const auto& indirectSpecular =
                *reinterpret_cast<const ffxDispatchDescDenoiserIndirectSpecular*>(specularHeader);
            LogRRDispatchSnapshot(dispatchDesc, directDiffuse, indirectSpecular, ambientOcclusion);
        }
        else
        {
            LOG_INFO("[RR_DIAG] single-signal dispatch: head={:#x} -> {} -> tail=0x0, frame={}, reset={}",
                     dispatchDesc.header.type,
                     GetSignalTypeName(_denoiseDiffuse ? _diffuseSignalDescType : _specularSignalDescType),
                     dispatchDesc.frameIndex, !!(dispatchDesc.flags & FFX_DENOISER_DISPATCH_RESET));
        }
        if (_denoiseDiffuse && _denoiseSpecular)
        {
            LogRRDiffuseHitDistanceProbe(NVSDK_NGX_Parameter_DLSSD_DiffuseHitDistance, _diffuseHitDistanceProbe,
                                         _diffuseHitDistanceBaseX, _diffuseHitDistanceBaseY,
                                         dispatchDesc.renderSize.width, dispatchDesc.renderSize.height, false);
            LogRRDiffuseHitDistanceProbe(NVSDK_NGX_Parameter_DLSSD_DiffuseRayDirectionHitDistance,
                                         _diffuseRayDirectionHitDistanceProbe, _diffuseRayDirectionHitDistanceBaseX,
                                         _diffuseRayDirectionHitDistanceBaseY, dispatchDesc.renderSize.width,
                                         dispatchDesc.renderSize.height, true);
            LogRREmissiveProbe(_emissiveProbe, _emissiveProbeCompatible, _emissiveProbeFromStreamline,
                               dispatchDesc.renderSize.width, dispatchDesc.renderSize.height);
            LogRRGBufferIdentityProbe(NVSDK_NGX_Parameter_GBuffer_MaterialId, _materialIdProbe,
                                      dispatchDesc.renderSize.width, dispatchDesc.renderSize.height);
            LogRRGBufferIdentityProbe(NVSDK_NGX_Parameter_GBuffer_ShadingModelId, _shadingModelIdProbe,
                                      dispatchDesc.renderSize.width, dispatchDesc.renderSize.height);
            StreamlineHooks::logRRSignalTagDiagnostics(dispatchDesc.renderSize.width, dispatchDesc.renderSize.height);
            StreamlineHooks::logSLTagInventoryDiagnostics(dispatchDesc.renderSize.width,
                                                          dispatchDesc.renderSize.height);
            StreamlineHooks::logRRNGXPointerDiagnostics();
            ResTrack_Dx12::LogRRScalarResourceCandidates(dispatchDesc.renderSize.width, dispatchDesc.renderSize.height);
            LOG_INFO(
                "[RR_DIAG] conversion snapshot: viewSource={}, projectionSource={}, handedness={}, depthInput={}, "
                "depthDirection={}, motionResolution={}, roughness={}, roughnessFloor={:.4f}, "
                "zeroRoughnessMaterialType=unified-type-1, "
                "specularHitDistance={} (invalid={}), diffuseHitDistance={}, diffuseDirectionHitDistance={}, "
                "emissiveInput={} (previewCompatible={})",
                _viewFromStreamline ? "Streamline" : "NGX",
                _projectionFromStreamline ? "StreamlineReconstruction" : "NGX", _isRightHanded ? "RH" : "LH",
                _isHWDepth ? "hardware" : "linear", DepthInverted() ? "reversed-Z" : "standard-Z",
                LowResMV() ? "render" : "output", _roughnessSource == RoughnessSource::Packed ? "packed" : "separate",
                _convDesc.RoughnessFloor,
                (_convDesc.Resources.InSpecHitDist || _convDesc.Resources.InSpecularRayDirectionHitDistance) ? "present"
                                                                                                             : "absent",
                _specularSignalDescType == FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR ? "negative" : "zero",
                _diffuseHitDistanceProbe ? "present" : "absent",
                _diffuseRayDirectionHitDistanceProbe ? "present" : "absent", _emissiveProbe ? "present" : "absent",
                _emissiveProbeCompatible);
        }
    }

    // A/B reference: push AMD's queried baseline instead of the fork's tuned values.
    // The INI/menu values are left untouched so toggling back restores them, and the
    // comparison below re-applies whichever source just became active.
    const bool useAmdDefaults = cfg.FfxDenoiserUseAmdDefaults.value_or_default();

    const auto updateConfiguration = [this, useAmdDefaults](const CustomOptional<float>& cfgValue, float amdDefault,
                                                            float& currentValue, FfxApiConfigureDenoiserKey key)
    {
        const float requestedValue = useAmdDefaults ? amdDefault : cfgValue.value_or_default();
        if (requestedValue == currentValue)
            return true;

        const float previousValue = currentValue;
        currentValue = requestedValue;

        const ffxReturnCode_t result = ApplyConfiguration(key);
        if (result == FFX_API_RETURN_OK)
            return true;

        // Retry on the next frame instead of treating the failed value as applied.
        currentValue = previousValue;
        LOG_ERROR("[RR_DIAG] RR 1.2 configure key {} failed: {}", static_cast<uint64_t>(key),
                  FfxApiProxy::ReturnCodeToString(result));
        return false;
    };

    if (!updateConfiguration(cfg.FfxDenoiserDisocThreshold, _denoiserAmdDefaults.m_DisocclusionThreshold,
                             _denoiserSettings.m_DisocclusionThreshold,
                             FFX_API_CONFIGURE_DENOISER_KEY_DISOCCLUSION_THRESHOLD) ||
        !updateConfiguration(cfg.FfxDenoiserCrossBlNormStr, _denoiserAmdDefaults.m_CrossBilateralNormalStrength,
                             _denoiserSettings.m_CrossBilateralNormalStrength,
                             FFX_API_CONFIGURE_DENOISER_KEY_CROSS_BILATERAL_NORMAL_STRENGTH) ||
        !updateConfiguration(cfg.FfxDenoiserStabilityBias, _denoiserAmdDefaults.m_StabilityBias,
                             _denoiserSettings.m_StabilityBias, FFX_API_CONFIGURE_DENOISER_KEY_STABILITY_BIAS) ||
        !updateConfiguration(cfg.FfxDenoiserMaxRadiance, _denoiserAmdDefaults.m_MaxRadiance,
                             _denoiserSettings.m_MaxRadiance, FFX_API_CONFIGURE_DENOISER_KEY_MAX_RADIANCE) ||
        !updateConfiguration(cfg.FfxDenoiserRadianceClip, _denoiserAmdDefaults.m_RadianceClipStdK,
                             _denoiserSettings.m_RadianceClipStdK,
                             FFX_API_CONFIGURE_DENOISER_KEY_RADIANCE_CLIP_STD_K) ||
        !updateConfiguration(cfg.FfxDenoiserGaussKernRelax, _denoiserAmdDefaults.m_GaussianKernelRelaxation,
                             _denoiserSettings.m_GaussianKernelRelaxation,
                             FFX_API_CONFIGURE_DENOISER_KEY_GAUSSIAN_KERNEL_RELAXATION))
    {
        return false;
    }

    const float requestedDebugDepthMax = std::clamp(cfg.FfxDenoiserDebugDepthMax.value_or_default(), 0.001f, 1024.0f);
    if (requestedDebugDepthMax != _denoiserSettings.m_DebugViewLinearDepthBounds.max)
    {
        const FfxApiFloatBounds previousBounds = _denoiserSettings.m_DebugViewLinearDepthBounds;
        _denoiserSettings.m_DebugViewLinearDepthBounds.max = requestedDebugDepthMax;

        const ffxReturnCode_t result =
            ApplyConfiguration(FFX_API_CONFIGURE_DENOISER_KEY_DEBUG_VIEW_LINEAR_DEPTH_BOUNDS);
        if (result != FFX_API_RETURN_OK)
        {
            _denoiserSettings.m_DebugViewLinearDepthBounds = previousBounds;
            LOG_ERROR("[RR_DIAG] RR 1.2 debug linear-depth bounds configure failed: {}",
                      FfxApiProxy::ReturnCodeToString(result));
            return false;
        }
    }

    ID3D12InfoQueue* infoQueue = nullptr;
    uint64_t firstD3D12Message = 0;
    const bool hasScopedInfoQueue =
        logDispatchSnapshot && Device && SUCCEEDED(Device->QueryInterface(IID_PPV_ARGS(&infoQueue)));

    if (hasScopedInfoQueue)
    {
        firstD3D12Message = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    }
    else if (logDispatchSnapshot)
    {
        LOG_INFO("[RR_DIAG][D3D12] InfoQueue unavailable. Dispatch return codes and device-removal reason are still "
                 "captured; enable the D3D12 debug layer before device creation for validation messages.");
    }

    ++_denoiserDispatchAttempts;
    if (_convDesc.DiagnosticsEnabled)
    {
        LOG_DEBUG("Dispatching FSR-RR 1.2 frame {} with {} + {}", dispatchDesc.frameIndex,
                  GetSignalTypeName(_diffuseSignalDescType), GetSignalTypeName(_specularSignalDescType));
    }
    const ffxReturnCode_t result = FfxApiProxy::D3D12_Dispatch(&_pDenoiserCtx, &dispatchDesc.header);
    _lastDispatchRequestedReset = resetRequested;

    if (hasScopedInfoQueue)
    {
        const std::string scope = std::format("frame {}", dispatchDesc.frameIndex);
        LogRRD3D12Messages(infoQueue, firstD3D12Message, scope);
    }

    if (result != FFX_API_RETURN_OK)
    {
        ++_denoiserDispatchFailures;
        LOG_ERROR("[RR_DIAG] dispatch failed: frame={}, result={}, attempts={}, successes={}, failures={}",
                  dispatchDesc.frameIndex, FfxApiProxy::ReturnCodeToString(result), _denoiserDispatchAttempts,
                  _denoiserDispatchSuccesses, _denoiserDispatchFailures);

        if (!hasScopedInfoQueue && Device && SUCCEEDED(Device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
        {
            const uint64_t messageCount = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
            const uint64_t firstRecentMessage = messageCount > 32u ? messageCount - 32u : 0u;
            const std::string scope = std::format("recent messages after failed frame {}", dispatchDesc.frameIndex);
            LogRRD3D12Messages(infoQueue, firstRecentMessage, scope);
        }

        if (Device)
        {
            const HRESULT removedReason = Device->GetDeviceRemovedReason();
            if (removedReason == S_OK)
            {
                LOG_INFO("[RR_DIAG] D3D12 device remains operational after the failed RR dispatch");
            }
            else
            {
                LOG_ERROR("[RR_DIAG] D3D12 device is removed: HRESULT={:#x}", static_cast<uint32_t>(removedReason));
                Util::GetDeviceRemovedReason(Device);
            }
        }

        if (result == FFX_API_RETURN_ERROR_RUNTIME_ERROR)
        {
            LOG_WARN("Trying to recover by recreating the feature");
            state.changeBackend[Handle()->Id] = true;
        }

        if (infoQueue)
            infoQueue->Release();

        return false;
    }

    ++_denoiserDispatchSuccesses;

    // Per-dispatch contract, sampled regularly enough to land next to the input
    // readback. It carries the two values the input probe cannot observe directly:
    // how far the camera actually moved, and the jitter pair the reprojection is
    // aligned against. Both are read here rather than inferred, because a reset or a
    // bypassed frame forces the depth delta in the motion vectors to zero and would
    // otherwise read as "the camera never moved".
    if ((_denoiserDispatchSuccesses % 60u) == 0u)
    {
        const XMFLOAT3 cameraPosition = GetFloat3Column(_invViewMatrix, 3);
        LOG_INFO("[RR_CFG] frame={}, reset={}, render={}x{}, depthBounds=[{:.4f}, {:.4f}], "
                 "jitter=[{:.6f}, {:.6f}] (prev [{:.6f}, {:.6f}]), cameraDelta=[{:.6f}, {:.6f}, {:.6f}], "
                 "cameraPosition=[{:.6f}, {:.6f}, {:.6f}], "
                 "depthInterpretation={}, resetFrame={}, historyValid={}, viewSource={}, "
                 "signals={} + {} (diffuseEnabled={}, specularEnabled={})",
                 dispatchDesc.frameIndex, resetRequested, dispatchDesc.renderSize.width, dispatchDesc.renderSize.height,
                 dispatchDesc.linearDepthBounds.min, dispatchDesc.linearDepthBounds.max, dispatchDesc.jitterOffsets.x,
                 dispatchDesc.jitterOffsets.y, _convDesc.JitterOffsets.z, _convDesc.JitterOffsets.w,
                 dispatchDesc.cameraPositionDelta.x, dispatchDesc.cameraPositionDelta.y,
                 dispatchDesc.cameraPositionDelta.z, cameraPosition.x, cameraPosition.y, cameraPosition.z,
                 _isHWDepth ? "hardware" : "linear", _isInReset, _hasDenoiserHistory,
                 _viewFromStreamline ? "Streamline" : "NGX", GetSignalTypeName(_diffuseSignalDescType),
                 GetSignalTypeName(_specularSignalDescType), _denoiseDiffuse, _denoiseSpecular);
    }

    if (_logNextDenoiserDispatch)
    {
        LOG_INFO("[RR_DIAG] first dispatch succeeded: frame={}, context={:X}, attempts={}, reset={}",
                 dispatchDesc.frameIndex, reinterpret_cast<uintptr_t>(_pDenoiserCtx), _denoiserDispatchAttempts,
                 resetRequested);
        _logNextDenoiserDispatch = false;
    }
    else if (resetTransition)
    {
        LOG_INFO("[RR_DIAG] reset dispatch succeeded: frame={}", dispatchDesc.frameIndex);
    }
    else if (_denoiserDispatchSuccesses == 60u || (_denoiserDispatchSuccesses % 600u) == 0u)
    {
        LOG_INFO("[RR_DIAG] dispatch stability milestone: {} successful dispatches, {} failures",
                 _denoiserDispatchSuccesses, _denoiserDispatchFailures);
    }

    if (infoQueue)
        infoQueue->Release();

    return true;
}

void FSRDFeatureDx12::CommitDenoiserHistory() noexcept
{
    _lastCamPos = GetFloat3Column(_invViewMatrix, 3);
    _prevViewMatrix = _viewMatrix;
    _previousDenoiserJitter = { _convDesc.JitterOffsets.x, _convDesc.JitterOffsets.y };
    _hasDenoiserHistory = true;
}

void FSRDFeatureDx12::RefreshHistoryDerivedInputs() noexcept
{
    // PrepareDenoiseConvInput derives these from _hasDenoiserHistory while it runs, and it
    // runs before the change checks in ConvertDenoiserBuffers that can invalidate it. Without
    // this the conversion pass is told its reprojection history is valid on the frame that
    // just abandoned it, and reprojects against a previous-frame depth produced under the
    // previous setting - which is the one frame the reset exists to avoid.
    if (!_hasDenoiserHistory || _isInReset)
    {
        _convDesc.MotionHistoryValid = false;
        _convDesc.JitterOffsets.z = _convDesc.JitterOffsets.x;
        _convDesc.JitterOffsets.w = _convDesc.JitterOffsets.y;
    }
}

// IEEE 754 half -> float for the probe readback (no DirectXPackedVector here).
static float ProbeHalfToFloat(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1u;
    uint32_t exponent = (h >> 10) & 0x1Fu;
    uint32_t mantissa = h & 0x3FFu;

    uint32_t bits = 0;
    if (exponent == 0u)
    {
        if (mantissa == 0u)
        {
            bits = sign << 31;
        }
        else
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
    }
    else if (exponent == 0x1Fu)
    {
        bits = (sign << 31) | 0x7F800000u | (mantissa << 13);
    }
    else
    {
        bits = (sign << 31) | ((exponent - 15u + 127u) << 23) | (mantissa << 13);
    }

    float result = 0.0f;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

bool FSRDFeatureDx12::SetDefaultConfiguration()
{
    for (int i = 0; i < DenoiserConfiguration::kKeyCount; i++)
    {
        const FfxApiConfigureDenoiserKey key = DenoiserConfiguration::GetIndexKey(i);
        const ffxReturnCode_t result = SetDefaultConfiguration(key);
        if (result != FFX_API_RETURN_OK)
        {
            LOG_ERROR("RR 1.2 default query for key {} failed: {}", static_cast<uint64_t>(key),
                      FfxApiProxy::ReturnCodeToString(result));
            return false;
        }
    }

    return true;
}

ffxReturnCode_t FSRDFeatureDx12::SetDefaultConfiguration(FfxApiConfigureDenoiserKey key)
{
    void* data = _denoiserSettings.GetData(key);
    if (!data)
        return FFX_API_RETURN_ERROR_PARAMETER;

    ffxQueryDescDenoiserGetDefaultKeyValue queryDesc = {
        .header = { .type = FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE },
        .key = (uint64_t) key,
        .count = 1u,
        .data = data
    };

    const ffxReturnCode_t code = FfxApiProxy::D3D12_Query(&_pDenoiserCtx, &queryDesc.header);
    return code;
}

ffxReturnCode_t FSRDFeatureDx12::ApplyConfiguration(FfxApiConfigureDenoiserKey key)
{
    const void* data = _denoiserSettings.GetData(key);
    if (!data)
        return FFX_API_RETURN_ERROR_PARAMETER;

    ffxConfigureDescDenoiserKeyValue configureDesc = { .header = { .type =
                                                                       FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE },
                                                       .key = (uint64_t) key,
                                                       .count = 1u,
                                                       .data = data };

    const ffxReturnCode_t code = FfxApiProxy::D3D12_Configure(&_pDenoiserCtx, &configureDesc.header);
    return code;
}
