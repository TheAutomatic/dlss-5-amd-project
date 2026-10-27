#include <pch.h>
#include <Config.h>
#include <Util.h>
#include <dlssnr/amd/AmdBridge.h>
#include <proxies/FfxApi_Proxy.h>
#include "FSR31Feature_Dx12.h"
#include "NVNGX_Parameter.h"
#include "MathUtils.h"

#define FFX_UPSCALER_VERSION_MAJOR 4
#define FFX_UPSCALER_VERSION_MINOR 0
#define FFX_UPSCALER_VERSION_PATCH 3

#define FFX_UPSCALER_MAKE_VERSION(major, minor, patch) (((major) << 22) | ((minor) << 12) | (patch))
#define FFX_UPSCALER_VERSION                                                                                           \
    FFX_UPSCALER_MAKE_VERSION(FFX_UPSCALER_VERSION_MAJOR, FFX_UPSCALER_VERSION_MINOR, FFX_UPSCALER_VERSION_PATCH)

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x0b)
struct ffxCreateContextDescUpscaleVersion
{
    ffxCreateContextDescHeader header;
    uint32_t version; ///< The version of the API the application was built against. This must be set to
                      ///< FFX_UPSCALER_VERSION.
};

using namespace OptiMath;

// Local port of the fork's TryResourceBarrier helpers: transition only when the
// configured override state exists.
static void TryResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                               const CustomOptional<int32_t, NoDefault>& InBeforeState,
                               D3D12_RESOURCE_STATES InAfterState)
{
    if (InCommandList != nullptr && InResource != nullptr && InBeforeState.has_value())
    {
        D3D12_RESOURCE_BARRIER desc = {};
        desc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        desc.Transition.pResource = InResource;
        desc.Transition.StateBefore = (D3D12_RESOURCE_STATES) InBeforeState.value();
        desc.Transition.StateAfter = InAfterState;
        InCommandList->ResourceBarrier(1, &desc);
    }
}

static void TryResourceBarrier(ID3D12GraphicsCommandList* InCommandList, ID3D12Resource* InResource,
                               D3D12_RESOURCE_STATES InBeforeState,
                               const CustomOptional<int32_t, NoDefault>& InAfterState)
{
    if (InCommandList != nullptr && InResource != nullptr && InAfterState.has_value())
    {
        D3D12_RESOURCE_BARRIER desc = {};
        desc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        desc.Transition.pResource = InResource;
        desc.Transition.StateBefore = InBeforeState;
        desc.Transition.StateAfter = (D3D12_RESOURCE_STATES) InAfterState.value();
        InCommandList->ResourceBarrier(1, &desc);
    }
}

// The AMD NR replacement arrives in NON_PIXEL_SHADER_RESOURCE, without ALLOW_RENDER_TARGET, and must
// remain there. The configurable colour barriers describe the original texture, not this one. The
// barrier window has no parameter table to ask, so PrepareUpscalerInput, which this feature and the
// FSR-RR feature built on it both run just before opening the window, records the feature whose Color
// is the replacement. Per thread, like the replacement itself, which lasts until Restore after the evaluate.
static thread_local const FSR31FeatureDx12* amdReplacementFeature = nullptr;

using InputResources = FSR31FeatureDx12::InputResources;

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

static void SetFfxUpscaleKeyValue(ffxContext* ctx, float& currentValue, const CustomOptional<float>& newValue,
                                  uint64_t key, const char* featureName)
{
    const float val = newValue.value_or_default();

    if (currentValue != val)
    {
        currentValue = val;

        ffxConfigureDescUpscaleKeyValue config {};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
        config.key = key;
        config.ptr = &currentValue;

        const ffxReturnCode_t result = FfxApiProxy::D3D12_Configure(ctx, &config.header);

        if (result != FFX_API_RETURN_OK)
            LOG_WARN("{} configure result: {}", featureName, (UINT) result);
    }
}

NVSDK_NGX_Parameter* FSR31FeatureDx12::SetParameters(NVSDK_NGX_Parameter* InParameters)
{
    InParameters->Set("OptiScaler.SupportsUpscaleSize", true);
    return InParameters;
}

FSR31FeatureDx12::FSR31FeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters)
    : FSR31Feature(InHandleId, InParameters), IFeature_Dx12(InHandleId, InParameters),
      IFeature(InHandleId, SetParameters(InParameters)), _isInReset(false), _inputBuffers({}), _upscalerOutput(nullptr)
{
    InParameters->Set("OptiScaler.SupportsUpscaleSize", true);

    // Initialize FFX API
    FfxApiProxy::InitFfxDx12();
    _moduleLoaded = FfxApiProxy::IsSRReady();

    if (_moduleLoaded)
        LOG_INFO("amd_fidelityfx_dx12.dll methods loaded!");
    else
        LOG_ERROR("can't load amd_fidelityfx_dx12.dll methods!");
}

inline FSR31FeatureDx12::~FSR31FeatureDx12()
{
    if (State::Instance().isShuttingDown)
        return;

    if (_upscaleCtx != nullptr)
        FfxApiProxy::D3D12_DestroyContext(&_upscaleCtx, NULL);
}

bool FSR31FeatureDx12::InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    LOG_DEBUG("FSR31FeatureDx12::InitInternal");

    if (IsInited())
        return true;

    // Attempt to create the FSR context. Helper shaders and the ImGui overlay are
    // created by IFeature_Dx12::Init once this succeeds.
    return InitFSR3(InParameters);
}

bool FSR31FeatureDx12::InitFSR3(const NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (!ModuleLoaded())
        return false;

    if (IsInited())
        return true;

    if (Device == nullptr)
    {
        LOG_ERROR("D3D12Device is null!");
        return false;
    }

    if (!CreateUpscalerContext(*InParameters))
        return false;

    // Update version info for UI/Logging
    auto& state = State::Instance();
    auto& cfg = *Config::Instance();
    auto version = state.ffxUpscalerVersionsDx12.names[cfg.FfxUpscalerIndex.value_or_default()];
    _name = "FSR";
    parse_version(version);

    SetInit(true);

    return true;
}

bool FSR31FeatureDx12::CreateUpscalerContext(const NVSDK_NGX_Parameter& ngxParams)
{
    // Get available versions for overrides
    if (!QueryUpscalerVersions())
        return false;

    ffxOverrideVersion vidOverride = { .header = { .type = FFX_API_DESC_TYPE_OVERRIDE_VERSION },
                                       .versionId = GetUpscalerOverrideID() };
    // Backend desc
    ffxCreateBackendDX12Desc backendDesc = 
    { 
        .header = 
        { 
            .type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12,
            .pNext = &vidOverride.header // Chain: Backend -> Override
        },
        .device = Device
    };
    ffxCreateContextDescUpscaleVersion contextVersion = {
        .header = { .type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, .pNext = &backendDesc.header },
        .version = FFX_UPSCALER_VERSION
    };
    // Chain: Context -> Version -> Backend -> Override
    _upscaleCtxDesc = { .header = { .type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
                                    .pNext = &contextVersion.header } };

    // Context description
    ConfigureUpscalerContext(ngxParams);

    LOG_DEBUG("_upscaleCtx!");

    {
        ScopedSkipHeapCapture skipHeapCapture {};

        // Final Context Creation
        auto ret = FfxApiProxy::D3D12_CreateContext(&_upscaleCtx, &_upscaleCtxDesc.header, NULL);

        if (ret != FFX_API_RETURN_OK)
        {
            LOG_ERROR("_upscaleCtx error: {0}", FfxApiProxy::ReturnCodeToString(ret));
            return false;
        }
    }

    return true;
}

void FSR31FeatureDx12::ConfigureUpscalerContext(const NVSDK_NGX_Parameter& ngxParams)
{
    auto& cfg = *Config::Instance();

    // Init context descriptor
    _upscaleCtxDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    _upscaleCtxDesc.flags = 0;

#ifdef _DEBUG
    LOG_INFO("Debug checking enabled!");
    _upscaleCtxDesc.fpMessage = FfxLogCallback;
    _upscaleCtxDesc.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
#endif

    // Map NGX flags to FFX context flags
    if (DepthInverted())
        _upscaleCtxDesc.flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;

    if (AutoExposure())
        _upscaleCtxDesc.flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;

    if (IsHdr())
        _upscaleCtxDesc.flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;

    if (JitteredMV())
        _upscaleCtxDesc.flags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;

    if (!LowResMV())
        _upscaleCtxDesc.flags |= FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;

    // Configurable flags (User overrides)
    if (cfg.FsrNonLinearColorSpace.value_or_default())
    {
        _upscaleCtxDesc.flags |= FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
        LOG_INFO("contextDesc.initFlags (NonLinearColorSpace) {0:b}", _upscaleCtxDesc.flags);
    }

    if (cfg.Fsr4EnableDebugView.value_or_default())
    {
        LOG_INFO("Debug view enabled!");
        _upscaleCtxDesc.flags |= 512; // FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
    }

    SetResolutionConfig();
}

void FSR31FeatureDx12::SetResolutionConfig()
{
    auto& cfg = *Config::Instance();

    // Handle Output Scaling Multiplier (Manual resizing of the output)
    if (cfg.OutputScalingEnabled.value_or_default() && LowResMV())
    {
        const float ssMulti = std::clamp(cfg.OutputScalingMultiplier.value_or_default(), 0.5f, 3.0f);
        cfg.OutputScalingMultiplier.set_volatile_value(ssMulti);

        _targetWidth = static_cast<uint32_t>(DisplayWidth() * ssMulti);
        _targetHeight = static_cast<uint32_t>(DisplayHeight() * ssMulti);
    }
    else
    {
        _targetWidth = DisplayWidth();
        _targetHeight = DisplayHeight();
    }

    // Extended limits: Support rendering at higher than display resolution
    if (cfg.ExtendedLimits.value_or_default() && RenderWidth() > DisplayWidth())
    {
        _upscaleCtxDesc.maxRenderSize.width = RenderWidth();
        _upscaleCtxDesc.maxRenderSize.height = RenderHeight();

        cfg.OutputScalingMultiplier.set_volatile_value(1.0f);

        // If output scaling active, let it handle downsampling
        if (cfg.OutputScalingEnabled.value_or_default() && LowResMV())
        {
            _upscaleCtxDesc.maxUpscaleSize.width = _upscaleCtxDesc.maxRenderSize.width;
            _upscaleCtxDesc.maxUpscaleSize.height = _upscaleCtxDesc.maxRenderSize.height;

            // update target res
            _targetWidth = _upscaleCtxDesc.maxRenderSize.width;
            _targetHeight = _upscaleCtxDesc.maxRenderSize.height;
        }
        else
        {
            _upscaleCtxDesc.maxUpscaleSize.width = DisplayWidth();
            _upscaleCtxDesc.maxUpscaleSize.height = DisplayHeight();
        }
    }
    else
    {
        _upscaleCtxDesc.maxRenderSize.width = TargetWidth() > DisplayWidth() ? TargetWidth() : DisplayWidth();
        _upscaleCtxDesc.maxRenderSize.height = TargetHeight() > DisplayHeight() ? TargetHeight() : DisplayHeight();
        _upscaleCtxDesc.maxUpscaleSize.width = TargetWidth();
        _upscaleCtxDesc.maxUpscaleSize.height = TargetHeight();
    }
}

bool FSR31FeatureDx12::QueryUpscalerVersions()
{
    ScopedSkipSpoofingGlobal skipSpoofingGlobal {};

    auto& state = State::Instance();

    // Get available FSR versions from the proxy
    ffxQueryDescGetVersions versionQuery {};
    versionQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versionQuery.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versionQuery.device = Device; // only for DirectX 12 applications

    // Get number of versions for allocation
    uint64_t versionCount = 0;
    versionQuery.outputCount = &versionCount;

    FfxApiProxy::D3D12_Query(nullptr, &versionQuery.header);

    // Store available FSR version IDs and names in vectors
    state.ffxUpscalerVersionsDx12.ids.resize(versionCount);
    state.ffxUpscalerVersionsDx12.names.resize(versionCount);
    versionQuery.versionIds = state.ffxUpscalerVersionsDx12.ids.data();
    versionQuery.versionNames = state.ffxUpscalerVersionsDx12.names.data();
    FfxApiProxy::D3D12_Query(nullptr, &versionQuery.header);

    return versionCount > 0;
}

uint64_t FSR31FeatureDx12::GetUpscalerOverrideID()
{
    auto& state = State::Instance();
    auto& cfg = *Config::Instance();

    // Select specific FSR version based on user config index
    if (cfg.FfxUpscalerIndex.value_or_default() < 0 ||
        cfg.FfxUpscalerIndex.value_or_default() >= state.ffxUpscalerVersionsDx12.ids.size())
        cfg.FfxUpscalerIndex.set_volatile_value(0);

    return state.ffxUpscalerVersionsDx12.ids[cfg.FfxUpscalerIndex.value_or_default()];
}

bool FSR31FeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters)
{
    LOG_FUNC();

    if (!IsInited())
        return false;

    auto& cfg = *Config::Instance();
    const auto& inParams = *InParameters;

    if (cfg.DADepthIsLinear.value_for_config_ignore_default() == std::nullopt)
        cfg.DADepthIsLinear.set_volatile_value(false);

    _isInReset = false;

    if (uint32_t value = 0; inParams.Get(NVSDK_NGX_Parameter_Reset, &value) == NVSDK_NGX_Result_Success)
        _isInReset = value == 1;

    // Resource Gathering
    ffxDispatchDescUpscale upscalerDesc = {};

    if (!PrepareUpscalerInput(InCommandList, inParams, upscalerDesc))
        return false;

    // Sets optional, configurable resource barriers, restored on scope exit
    ScopedConfigurableBarriers scopedBarriers(*this, InCommandList);

    const bool isUpscalerReady = DispatchUpscaler(InCommandList, upscalerDesc);

    _frameCount++;
    return isUpscalerReady;
}

bool FSR31FeatureDx12::PrepareUpscalerInput(ID3D12GraphicsCommandList* InCommandList,
                                            const NVSDK_NGX_Parameter& inParams, ffxDispatchDescUpscale& upscalerDesc)
{
    auto& state = State::Instance();
    auto& cfg = *Config::Instance();

    _inputBuffers = {};

    // HasReplacement only compares the table's address with the one AmdBridge substituted into.
    amdReplacementFeature =
        DlssNr::AmdBridge::HasReplacement(const_cast<NVSDK_NGX_Parameter*>(&inParams)) ? this : nullptr;

    ConfigureUpscaler(inParams, upscalerDesc);

    // Resolve Output Chain (Main -> Scaler -> RCAS)
    // Track the resource FSR writes into
    if (!SetUpscalerTarget(InCommandList, inParams))
        return false;

    // Main inputs
    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_Color, _inputBuffers.Color))
        return false;
    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_MotionVectors, _inputBuffers.MotionVectors))
        return false;
    if (!TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_Depth, _inputBuffers.Depth) && LowResMV())
        return false;

    // Optional Resources
    TryGetNGXVoidPointer(inParams, OptiKeys::FSR_TransparencyAndComp, _inputBuffers.TransparencyMask);
    TryGetNGXVoidPointer(inParams, OptiKeys::FSR_Reactive, _inputBuffers.ReactiveMask);
    TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask,
                         _inputBuffers.DlssBiasMaskFallback);
    TryGetNGXVoidPointer(inParams, NVSDK_NGX_Parameter_ExposureTexture, _inputBuffers.ExposureMap);

    // If not AutoExposure, we must have an exposure texture. If missing, force AutoExposure reset.
    if (!AutoExposure() && !_inputBuffers.ExposureMap)
    {
        LOG_DEBUG("AutoExposure disabled but ExposureTexture is missing. Forcing AutoExposure and re-initializing.");
        state.autoExposure = true;
        state.changeBackend[Handle()->Id] = true;
        // This instance was created without auto exposure, so it cannot dispatch
        // correctly until the requested backend recreation has happened. Returning
        // success here would send a zero-initialized dispatch descriptor to FFX.
        return false;
    }

    // Resolve Reactive & Transparency Masks
    GetReactiveAndTransparencyMasks(InCommandList, _inputBuffers);

    // Map inputs to descriptor
    upscalerDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    upscalerDesc.commandList = InCommandList;

    // Mandatory Inputs
    upscalerDesc.color = ffxApiGetResourceDX12(_inputBuffers.Color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    upscalerDesc.motionVectors =
        ffxApiGetResourceDX12(_inputBuffers.MotionVectors, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    upscalerDesc.depth = ffxApiGetResourceDX12(_inputBuffers.Depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);

    // Output
    upscalerDesc.output = ffxApiGetResourceDX12(_upscalerOutput, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);

    // Reactive / Transparency
    if (_inputBuffers.ReactiveMask)
    {
        LOG_DEBUG("Assigning Reactive Mask");
        upscalerDesc.reactive = ffxApiGetResourceDX12(_inputBuffers.ReactiveMask, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    }

    if (_inputBuffers.TransparencyMask)
    {
        LOG_DEBUG("Assigning Transparency Mask");
        upscalerDesc.transparencyAndComposition =
            ffxApiGetResourceDX12(_inputBuffers.TransparencyMask, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    }

    // Exposure
    if (AutoExposure())
        LOG_DEBUG("Using AutoExposure");
    else if (_inputBuffers.ExposureMap)
    {
        LOG_DEBUG("Using Exposure Texture");
        upscalerDesc.exposure = ffxApiGetResourceDX12(_inputBuffers.ExposureMap, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    }

    // State Tracking / Debug
    _hasColor = upscalerDesc.color.resource != nullptr;
    _hasDepth = upscalerDesc.depth.resource != nullptr;
    _hasMV = upscalerDesc.motionVectors.resource != nullptr;
    _hasExposure = upscalerDesc.exposure.resource != nullptr;
    _hasTM = upscalerDesc.transparencyAndComposition.resource != nullptr;
    _accessToReactiveMask =
        _inputBuffers.ReactiveMask != nullptr; // Keep original logic tracking if "native" mask existed
    _hasOutput = upscalerDesc.output.resource != nullptr;

    // FSR 4 Format Fixes
    if (Version().major >= 4)
    {
        ffxResolveTypelessFormat(upscalerDesc.color.description.format);
        ffxResolveTypelessFormat(upscalerDesc.depth.description.format);
        ffxResolveTypelessFormat(upscalerDesc.motionVectors.description.format);
        ffxResolveTypelessFormat(upscalerDesc.exposure.description.format);
        ffxResolveTypelessFormat(upscalerDesc.transparencyAndComposition.description.format);
        ffxResolveTypelessFormat(upscalerDesc.output.description.format);
    }

    return true;
}

bool FSR31FeatureDx12::SetUpscalerTarget(ID3D12GraphicsCommandList* InCommandList, const NVSDK_NGX_Parameter& inParams)
{
    // IFeature_Dx12::Evaluate redirects the Output parameter to an intermediate
    // buffer when post-processing is active, so the upscaler simply writes to
    // whatever the parameter table points at.
    _upscalerOutput = nullptr;

    return TryGetLoggedResource(inParams, NVSDK_NGX_Parameter_Output, _upscalerOutput);
}

void FSR31FeatureDx12::ConfigureUpscaler(const NVSDK_NGX_Parameter& inParams, ffxDispatchDescUpscale& upscalerDesc)
{
    auto& state = State::Instance();
    auto& cfg = *Config::Instance();

    // Configure Debug Flags
    // Handle FSR 4.0 specific debug view logic or standard FSR debug flags
    if (cfg.FsrDebugView.value_or_default() &&
        (Version() < feature_version { 4, 0, 0 } || cfg.Fsr4EnableDebugView.value_or_default()))
    {
        upscalerDesc.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
    }

    // Color Space Configuration
    if (cfg.FsrNonLinearPQ.value_or_default())
        upscalerDesc.flags |= FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ;
    else if (cfg.FsrNonLinearSRGB.value_or_default())
        upscalerDesc.flags |= FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;

    // Retrieve Jitter Offsets
    inParams.Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &upscalerDesc.jitterOffset.x);
    inParams.Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &upscalerDesc.jitterOffset.y);

    LOG_DEBUG("Jitter Offset: {0}x{1}", upscalerDesc.jitterOffset.x, upscalerDesc.jitterOffset.y);

    // Sharpening
    if (cfg.OverrideSharpness.value_or_default())
        _sharpness = cfg.Sharpness.value_or_default();
    else
        _sharpness = GetSharpness(&inParams);

    // If RCAS is enabled externally, disable FSR built-in sharpening to avoid double sharpening
    if (cfg.RcasEnabled.value_or_default())
    {
        upscalerDesc.enableSharpening = false;
        upscalerDesc.sharpness = 0.0f;
    }
    else
    {
        if (_sharpness > 1.0f)
            _sharpness = 1.0f;

        upscalerDesc.enableSharpening = _sharpness > 0.0f;
        upscalerDesc.sharpness = _sharpness;
    }

    // Force enable RCAS when in FSR4 debug view mode
    // it crashes when sharpening is disabled
    // Debug view expects RCAS output (not sure why)
    if (Version() >= feature_version { 4, 0, 2 } && cfg.FsrDebugView.value_or_default() &&
        cfg.Fsr4EnableDebugView.value_or_default() && !upscalerDesc.enableSharpening)
    {
        upscalerDesc.enableSharpening = true;
        upscalerDesc.sharpness = 0.01f;
    }

    // Get reset flag
    upscalerDesc.reset = _isInReset;

    GetRenderResolution(&inParams, &upscalerDesc.renderSize.width, &upscalerDesc.renderSize.height);
    LOG_DEBUG("Input Resolution: {0}x{1}", upscalerDesc.renderSize.width, upscalerDesc.renderSize.height);

    // Motion Vector Scaling
    float MVScaleX = 1.0f;
    float MVScaleY = 1.0f;

    if (inParams.Get(NVSDK_NGX_Parameter_MV_Scale_X, &MVScaleX) == NVSDK_NGX_Result_Success &&
        inParams.Get(NVSDK_NGX_Parameter_MV_Scale_Y, &MVScaleY) == NVSDK_NGX_Result_Success)
    {
        upscalerDesc.motionVectorScale.x = MVScaleX;
        upscalerDesc.motionVectorScale.y = MVScaleY;
    }
    else
    {
        LOG_WARN("Can't get motion vector scales!");
        upscalerDesc.motionVectorScale.x = MVScaleX;
        upscalerDesc.motionVectorScale.y = MVScaleY;
    }

    LOG_DEBUG("Sharpness: {0}", upscalerDesc.sharpness);

    // Camera & View Parameters with fallbacks

    // Not explicitly set in the DLSS path. DLSS assumes near == 0 and far == 1 or the inverse.
    if (!TryGetToggleableNGXParam(inParams, OptiKeys::FSR_NearPlane, cfg.FsrUseFsrInputValues, upscalerDesc.cameraNear))
    {
        upscalerDesc.cameraNear = cfg.FsrCameraNear.value_or_default();
    }

    if (!TryGetToggleableNGXParam(inParams, OptiKeys::FSR_FarPlane, cfg.FsrUseFsrInputValues, upscalerDesc.cameraFar))
    {
        upscalerDesc.cameraFar = cfg.FsrCameraFar.value_or_default();
    }

    // Not being set in DLSS or XeSS input paths. Inverse VP matrices may be deprecated in modern DLSS.
    if (!TryGetToggleableNGXParam(inParams, OptiKeys::FSR_CameraFovVertical, cfg.FsrUseFsrInputValues,
                                  upscalerDesc.cameraFovAngleVertical))
    {
        if (cfg.FsrVerticalFov.has_value())
            upscalerDesc.cameraFovAngleVertical = GetRadiansFromDeg(cfg.FsrVerticalFov.value());
        else if (cfg.FsrHorizontalFov.value_or_default() > 0.0f)
        {
            const float hFovRad = GetRadiansFromDeg(cfg.FsrHorizontalFov.value());
            upscalerDesc.cameraFovAngleVertical =
                GetVerticalFovFromHorizontal(hFovRad, (float) TargetWidth(), (float) TargetHeight());
        }
        else
            upscalerDesc.cameraFovAngleVertical = GetRadiansFromDeg(60);
    }

    // Use game deltatime or fall back to internal measurements
    if (!TryGetToggleableNGXParam(inParams, OptiKeys::FSR_FrameTimeDelta, cfg.FsrUseFsrInputValues,
                                  upscalerDesc.frameTimeDelta))
    {
        if (inParams.Get(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, &upscalerDesc.frameTimeDelta) !=
                NVSDK_NGX_Result_Success ||
            upscalerDesc.frameTimeDelta < 1.0f)
        {
            upscalerDesc.frameTimeDelta = (float) GetDeltaTime();
        }
    }

    LOG_DEBUG("FrameTimeDeltaInMsec: {0}", upscalerDesc.frameTimeDelta);

    if (!TryGetToggleableNGXParam(inParams, OptiKeys::FSR_ViewSpaceToMetersFactor, cfg.FsrUseFsrInputValues,
                                  upscalerDesc.viewSpaceToMetersFactor))
    {
        upscalerDesc.viewSpaceToMetersFactor = 0.0f;
    }

    upscalerDesc.upscaleSize.width = TargetWidth();
    upscalerDesc.upscaleSize.height = TargetHeight();

    if (inParams.Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &upscalerDesc.preExposure) != NVSDK_NGX_Result_Success)
        upscalerDesc.preExposure = 1.0f;

    // FSR 3.1+ Advanced Configuration
    // Apply volatile configuration keys using FfxApiProxy::D3D12_Configure

    // Velocity Factor (FSR 3.1.1+)
    if (Version() >= feature_version { 3, 1, 1 })
    {
        SetFfxUpscaleKeyValue(&_upscaleCtx, _velocity, cfg.FsrVelocity, FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR,
                              "Velocity");
    }

    // Reactiveness, Shading, and Accumulation (FSR 3.1.4+)
    if (Version() >= feature_version { 3, 1, 4 })
    {
        SetFfxUpscaleKeyValue(&_upscaleCtx, _reactiveScale, cfg.FsrReactiveScale,
                              FFX_API_CONFIGURE_UPSCALE_KEY_FREACTIVENESSSCALE, "Reactive Scale");
        SetFfxUpscaleKeyValue(&_upscaleCtx, _shadingScale, cfg.FsrShadingScale,
                              FFX_API_CONFIGURE_UPSCALE_KEY_FSHADINGCHANGESCALE, "Shading Scale");
        SetFfxUpscaleKeyValue(&_upscaleCtx, _accAddPerFrame, cfg.FsrAccAddPerFrame,
                              FFX_API_CONFIGURE_UPSCALE_KEY_FACCUMULATIONADDEDPERFRAME, "Acc. Add Per Frame");
        SetFfxUpscaleKeyValue(&_upscaleCtx, _minDisOccAcc, cfg.FsrMinDisOccAcc,
                              FFX_API_CONFIGURE_UPSCALE_KEY_FMINDISOCCLUSIONACCUMULATION, "Min Disocclusion Acc.");
    }

    // Output Scaling Override
    if (cfg.OutputScalingEnabled.value_or_default())
    {
        // If external output scaling is enabled, we may need to adjust the reported upscale size
        if (inParams.Get(OptiKeys::FSR_UpscaleWidth, &upscalerDesc.upscaleSize.width) == NVSDK_NGX_Result_Success)
            upscalerDesc.upscaleSize.width *= static_cast<uint32_t>(cfg.OutputScalingMultiplier.value_or_default());

        if (inParams.Get(OptiKeys::FSR_UpscaleHeight, &upscalerDesc.upscaleSize.height) == NVSDK_NGX_Result_Success)
            upscalerDesc.upscaleSize.height *= static_cast<uint32_t>(cfg.OutputScalingMultiplier.value_or_default());
    }
}

bool FSR31FeatureDx12::DispatchUpscaler(ID3D12GraphicsCommandList* InCommandList,
                                        const ffxDispatchDescUpscale& fsrParams)
{
    auto& state = State::Instance();

    LOG_DEBUG("Dispatching FSR...");
    const ffxReturnCode_t result = FfxApiProxy::D3D12_Dispatch(&_upscaleCtx, &fsrParams.header);

    if (result != FFX_API_RETURN_OK)
    {
        LOG_ERROR("_dispatch error: {0}", FfxApiProxy::ReturnCodeToString(result));

        if (result == FFX_API_RETURN_ERROR_RUNTIME_ERROR)
        {
            LOG_WARN("Trying to recover by recreating the feature");
            state.changeBackend[Handle()->Id] = true;
        }

        return false;
    }

    return true;
}

void FSR31FeatureDx12::GetReactiveAndTransparencyMasks(ID3D12GraphicsCommandList* InCommandList, InputResources& inputs)
{
    auto& cfg = *Config::Instance();
    ID3D12Resource* activeReactiveMask = nullptr;
    ID3D12Resource* activeTransparencyMask = nullptr;

    // FSRD's conversion path reads the title's DLSS bias mask independently of
    // whether FSR later selects it as the active reactive mask. Transition it as
    // soon as it is acquired so native FSR masks and DisableReactiveMask cannot
    // leave the conversion sampling a UAV or render-target state.
    TryResourceBarrier(InCommandList, inputs.DlssBiasMaskFallback, cfg.MaskResourceBarrier,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (!cfg.DisableReactiveMask.value_or(inputs.ReactiveMask == nullptr && inputs.DlssBiasMaskFallback == nullptr))
    {
        // Prefer explicit FSR masks
        if (inputs.TransparencyMask)
            activeTransparencyMask = inputs.TransparencyMask;
        if (inputs.ReactiveMask)
            activeReactiveMask = inputs.ReactiveMask;

        // Fallback to DLSS Bias mask if FSR reactive is missing
        if (!activeReactiveMask && inputs.DlssBiasMaskFallback)
        {
            LOG_DEBUG("Using DLSS Input Bias mask as fallback...");
            cfg.DisableReactiveMask.set_volatile_value(false);

            // Handle Bias generation (Compute Shader)
            if (cfg.DlssReactiveMaskBias.value_or_default() > 0.0f && Bias->IsInit() && Bias->CanRender())
            {
                if (Bias->CreateBufferResource(Device, inputs.DlssBiasMaskFallback,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
                {
                    Bias->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                    if (Bias->Dispatch(InCommandList, inputs.DlssBiasMaskFallback,
                                       cfg.DlssReactiveMaskBias.value_or_default(), Bias->Buffer()))
                    {
                        Bias->SetBufferState(InCommandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        activeReactiveMask = Bias->Buffer(); // Use the processed bias buffer
                    }
                    else
                    {
                        LOG_DEBUG("Skipping reactive mask, Bias: {0}, Bias Init: {1}, Bias CanRender: {2}",
                                  cfg.DlssReactiveMaskBias.value_or_default(), Bias->IsInit(), Bias->CanRender());
                    }
                }
            }

            // Use DLSS mask for Transparency if FSR Transparency is missing and config allows
            if (!activeTransparencyMask && cfg.FsrUseMaskForTransparency.value_or_default())
                activeTransparencyMask = inputs.DlssBiasMaskFallback;
        }
    }

    inputs.TransparencyMask = activeTransparencyMask;
    inputs.ReactiveMask = activeReactiveMask;
}

void FSR31FeatureDx12::SetConfigurableBarriers(ID3D12GraphicsCommandList* InCommandList)
{
    const auto& state = State::Instance();
    auto& cfg = *Config::Instance();

    // Handle UE Quirks. The colour state is recorded on replacement frames too: AmdBridge reads it as
    // the game colour's state.
    if (state.NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL || state.gameQuirks & GameQuirk::ForceUnrealEngine)
    {
        if (!cfg.ColorResourceBarrier.has_value())
            cfg.ColorResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_RENDER_TARGET);

        if (!cfg.MVResourceBarrier.has_value())
            cfg.MVResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Transition FSR inputs to SRVs for reading
    if (amdReplacementFeature != this)
        TryResourceBarrier(InCommandList, _inputBuffers.Color, cfg.ColorResourceBarrier,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TryResourceBarrier(InCommandList, _inputBuffers.MotionVectors, cfg.MVResourceBarrier,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    TryResourceBarrier(InCommandList, _inputBuffers.Depth, cfg.DepthResourceBarrier,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (_inputBuffers.ExposureMap && !AutoExposure())
        TryResourceBarrier(InCommandList, _inputBuffers.ExposureMap, cfg.ExposureResourceBarrier,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Transition output to UAV for writing
    TryResourceBarrier(InCommandList, _upscalerOutput, cfg.OutputResourceBarrier,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void FSR31FeatureDx12::ResetConfigurableBarriers(ID3D12GraphicsCommandList* InCommandList)
{
    const auto& cfg = *Config::Instance();

    // Restore Barriers
    if (amdReplacementFeature != this)
        TryResourceBarrier(InCommandList, _inputBuffers.Color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           cfg.ColorResourceBarrier);
    TryResourceBarrier(InCommandList, _inputBuffers.MotionVectors, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       cfg.MVResourceBarrier);
    TryResourceBarrier(InCommandList, _inputBuffers.Depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       cfg.DepthResourceBarrier);
    TryResourceBarrier(InCommandList, _upscalerOutput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       cfg.OutputResourceBarrier);

    if (_inputBuffers.ExposureMap && !AutoExposure())
        TryResourceBarrier(InCommandList, _inputBuffers.ExposureMap, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                           cfg.ExposureResourceBarrier);

    // GetReactiveAndTransparencyMasks transitions only the title-owned DLSS bias
    // fallback. ReactiveMask may alias that same resource (or an internal Bias output),
    // so restoring ReactiveMask as well would either emit the same barrier twice or
    // invent a reverse transition that had no matching forward transition.
    if (_inputBuffers.DlssBiasMaskFallback)
        TryResourceBarrier(InCommandList, _inputBuffers.DlssBiasMaskFallback,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, cfg.MaskResourceBarrier);
}
