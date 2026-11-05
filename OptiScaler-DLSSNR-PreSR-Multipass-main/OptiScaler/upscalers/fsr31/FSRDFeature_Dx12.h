#pragma once
#include "FSR31Feature_Dx12.h"
#include "hooks/Streamline_Hooks.h"
#include "shaders/fsrd_preprocess/FSRDPreprocessor_Dx12.h"
#include <array>
#include <dlssnr/submission/GpuBatchTracker.h>
#include <atomic>
#include <mutex>
#include "FSRDSignalPolicy.h"
#include "FSRDCameraMatrices.h"
#include "FSRDRawDepthProbe.h"
#include "FSRDRetryPolicy.h"
#include "gpu_time/FSRDStageTimings_Dx12.h"
#include <DirectXMath.h>

/**
 * @brief Unfied denoiser-upscaler utilising AMD FSR Ray Regeneration and Super Resolution with
 * DLSS-RR inputs. Extends FSR 3.1+ upscaler implementation.
 */
class FSRDFeatureDx12 : public FSR31FeatureDx12
{
  public:
    using FSRDConvDesc = FSRDPreprocessor_Dx12::ConversionDesc;

    FSRDFeatureDx12(uint32_t InHandleId, NVSDK_NGX_Parameter* InParameters);

    ~FSRDFeatureDx12();

    feature_version Version() override { return FSR31FeatureDx12::Version(); }

    Upscaler GetUpscalerType() const override { return Upscaler::FSR_RR; }

    bool InitInternal(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*) override;
    bool EvaluateFallback(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*) override;
    void OnEvaluationStarting(NVSDK_NGX_Parameter*) override;
    void OnEvaluationFinished(bool success) override;
    void CopyRecreationParameters(NVSDK_NGX_Parameter*) const override;

    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

    bool RecordingsRetired();
    static void DestroyWhenRetired(std::unique_ptr<FSRDFeatureDx12> feature);
    static void ReapDeferredFeatures();

    // Bits 0/1: native specular/diffuse ray guide; bit 2: validated frame seen.
    // Bits 8..11: effective radiance signals, after safe fallback for missing guides.
    bool ReadRayRegenerationDiagnostics(FSRDRuntimeSnapshot& snapshot) const override
    {
        std::lock_guard lock(_runtimeMutex);
        snapshot = _publishedRuntime;
        snapshot.signalStatus = _signalStatus.load(std::memory_order_relaxed);
        snapshot.timings = _stageTimings.GetSnapshot();
        return true;
    }

  private:
    using RRResult = FSRD::RRResult;
    FSRD::RRRetryPolicy _rrRetryPolicy;
    bool _rrFailureRecordedThisEvaluation = false;
    FSRDRawDepthProbe _rawDepthProbe;
    RRResult ClassifyRayRegenerationFailure(ffxReturnCode_t result, bool dynamicInput) const noexcept;

    struct DenoiserConfiguration
    {
        static constexpr uint32_t kScalarCount = FFX_API_CONFIGURE_DENOISER_KEY_DISOCCLUSION_THRESHOLD;
        static constexpr uint32_t kKeyCount = FFX_API_CONFIGURE_DENOISER_KEY_DEBUG_VIEW_LINEAR_DEPTH_BOUNDS;

        // Ordered by FfxApiConfigureDenoiserKey
        union
        {
            struct
            {
                float m_CrossBilateralNormalStrength;
                float m_StabilityBias;
                float m_MaxRadiance;
                float m_RadianceClipStdK;
                float m_GaussianKernelRelaxation;
                float m_DisocclusionThreshold;
            };

            float ScalarValues[kScalarCount];
        };

        FfxApiFloatBounds m_DebugViewLinearDepthBounds;

        static FfxApiConfigureDenoiserKey GetIndexKey(int index)
        {
            index = std::clamp(index + 1, 1, static_cast<int>(kKeyCount));
            return static_cast<FfxApiConfigureDenoiserKey>(index);
        }

        void* GetData(FfxApiConfigureDenoiserKey key)
        {
            if (key == FFX_API_CONFIGURE_DENOISER_KEY_DEBUG_VIEW_LINEAR_DEPTH_BOUNDS)
                return &m_DebugViewLinearDepthBounds;

            const int index = static_cast<int>(key) - 1;
            return index >= 0 && index < static_cast<int>(kScalarCount)
                ? &ScalarValues[index]
                : nullptr;
        }
    };

    std::vector<DlssNr::Submission::GpuBatch::Tracker::Ticket> _recordingTickets;
    struct RetainedInput
    {
        DlssNr::Submission::GpuBatch::Tracker::Ticket ticket;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    };
    std::vector<RetainedInput> _retainedInputs;
    void RetainFrameInputs(ID3D12Resource* inspector);
    static inline uint32_t _observedMaxRenderWidth = 0;
    static inline uint32_t _observedMaxRenderHeight = 0;
    ffxContext _pDenoiserCtx;
    ffxCreateContextDescDenoiser _denoiserCtxDesc;
    // Version parsed from the selected RR provider name. Kept separate from
    // FSR31Feature::_version so the SR upscaler version that Version() reports -
    // and every Version()-gated SR behaviour relies on - is never overwritten by
    // the denoiser provider version on context (re)creation.
    feature_version _denoiserVersion {};
    DenoiserConfiguration _denoiserSettings;
    // AMD's queried baseline, captured before the per-frame configure pass starts
    // overwriting _denoiserSettings with the INI values. Retained so the A/B switch
    // can restore it without recreating the context.
    DenoiserConfiguration _denoiserAmdDefaults {};
    ffxStructType_t _diffuseSignalDescType = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE;
    ffxStructType_t _specularSignalDescType = FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR;
    // Single-signal denoising (DenoiseDiffuse/DenoiseSpecular ini keys): disabled
    // signals are neither declared at context creation nor dispatched.
    bool _denoiseDiffuse = true;
    bool _denoiseSpecular = true;
    // The context's RR layout uses current validated guides; historical observations
    // remain diagnostics only and never determine a replacement context's layout.
    FSRDSignals::Plan _plan;
    FSRDSignals::Guides _guides;
    bool _signalsObserved = false;
    bool _ambientOcclusionEnabled = false;
    bool _specularOcclusionEnabled = false;
    // Unsupported-albedo recovery: the context also denoises the unmodulated specular
    // share as RR direct specular, and composition blends toward it where the albedo's
    // structure is absent from the light (water over a visible sea floor in the G-buffer).
    // The layout reserves the slot at context creation; activation can change live.
    bool _unsupportedAlbedoRecovery = false;
    bool _extraSpecularSignal = false;
    bool _extraDiffuseSignal = false;
    uint32_t _signalMask = 0;
    uint32_t _appliedApproximationMask = ~0u;
    std::atomic<uint32_t> _signalStatus { 0 };
    FSRDStageTimings _stageTimings;
    mutable std::mutex _runtimeMutex;
    FSRDRuntimeSnapshot _runtime, _publishedRuntime;
    bool _rrInitialized = false;
    uint64_t _evaluationNumber = 0;
    std::string _rrFailure;
    FSRDRuntimeSnapshot::Step _failedStep = FSRDRuntimeSnapshot::Inputs;

    RRResult EvaluateRayRegeneration(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*);
    void FailRayRegeneration(RRResult result, const char* reason);
    ffxDispatchDescDenoiserIndirectDiffuse _indirectDiffuseSignal {};
    ffxDispatchDescDenoiserDirectSpecular _directSpecularSignal {};
    Microsoft::WRL::ComPtr<ID3D12Resource> _ambientOcclusionNoisy;
    Microsoft::WRL::ComPtr<ID3D12Resource> _ambientOcclusionDenoised;
    D3D12_RESOURCE_STATES _ambientOcclusionNoisyState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES _ambientOcclusionDenoisedState = D3D12_RESOURCE_STATE_COMMON;

    enum class RoughnessSource : uint8_t
    {
        Unknown,
        Separate,
        Packed
    };

    // Depth-type interpretation. NGX reports it at creation, but a title that
    // publishes nothing leaves it at Linear, and reading a hardware depth buffer as
    // linear distance collapses every scene depth into [near, 1]. Keep the reported
    // value and its presence separate from the effective one so the user can
    // override it and so the log can say which of the two is in play.
    // Latched across instances. NGX publishes the depth type only on a creation
    // carrying DLSSD create params, so a recreation can arrive without it; the type
    // itself does not change mid-session, and losing it drops the decision back onto
    // an inference that has to guess.
    // Static on purpose, not by accident: the depth type is a property of the title, not of one
    // feature instance. NGX publishes it only on a creation that carries DLSSD create params, so
    // a recreation - a resolution or preset change, or a backend switch - can arrive without it,
    // and re-deriving per instance would lose a declaration the title already made. See the
    // reuse path in the constructor for what that costs when it is lost.
    static bool s_ngxDepthTypeSeen;
    static bool s_ngxReportedHWDepth;

    bool _isHWDepth = false;
    bool _ngxReportedHWDepth = false;
    bool _hasNGXDepthType = false;
    int _appliedHardwareDepth = -1;
    RoughnessSource _roughnessSource = RoughnessSource::Unknown;

    FSRDConvDesc _convDesc;
    // Optional title inputs are bound only after format, region and lifecycle validation.
    ID3D12Resource* _diffuseHitDistanceProbe = nullptr;
    ID3D12Resource* _diffuseRayDirectionHitDistanceProbe = nullptr;
    ID3D12Resource* _emissiveProbe = nullptr;
    ID3D12Resource* _materialIdProbe = nullptr;
    ID3D12Resource* _shadingModelIdProbe = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> _emissiveTaggedResource;
    // Current-frame selection; RetainFrameInputs extends these references through the
    // recording ticket's actual GPU retirement rather than the next CPU evaluation.
    Microsoft::WRL::ComPtr<ID3D12Resource> _specularHitDistanceTaggedResource;
    // Retained per frame: the tag path hands back a resource whose lifetime the caller
    // must hold for as long as the command list that reads it.
    Microsoft::WRL::ComPtr<ID3D12Resource> _titleLinearDepthTaggedResource;
    Microsoft::WRL::ComPtr<ID3D12Resource> _normalRoughnessTaggedResource;
    Microsoft::WRL::ComPtr<ID3D12Resource> _responsivityMaskTaggedResource;
    Microsoft::WRL::ComPtr<ID3D12Resource> _specularRayDirectionHitDistanceTaggedResource;
    // Instance-local suppression state for the responsivity binding log. Function-static
    // state races when multiple feature instances evaluate concurrently and lets one instance
    // suppress another instance's first diagnostic.
    ID3D12Resource* _loggedResponsivityMask = nullptr;
    uint64_t _loggedResponsivityWidth = 0;
    uint32_t _loggedResponsivityHeight = 0;
    DXGI_FORMAT _loggedResponsivityViewFormat = DXGI_FORMAT_UNKNOWN;
    float _loggedResponsivityThreshold = -1.0f;
    bool _loggedResponsivityInvert = false;
    std::array<uint64_t, static_cast<size_t>(RRTaggedSignal::Count)>
        _lastConsumedSLTagUpdates {};
    std::array<uint32_t, static_cast<size_t>(RRTaggedSignal::Count)>
        _lastConsumedSLTagFrames {};
    bool _emissiveProbeCompatible = false;
    bool _emissiveProbeFromStreamline = false;
    uint32_t _diffuseHitDistanceBaseX = 0;
    uint32_t _diffuseHitDistanceBaseY = 0;
    uint32_t _diffuseRayDirectionHitDistanceBaseX = 0;
    uint32_t _diffuseRayDirectionHitDistanceBaseY = 0;
    DirectX::XMFLOAT3 _lastCamPos {}; // Last successfully dispatched world-space camera position
    DirectX::XMFLOAT2 _previousDenoiserJitter {};
    int _appliedFloorEnabled = -1;
    float _appliedFloorDetailPreservation = -1.0f;
    float _appliedFloorHandoverAnchorClamp = -1.0f;
    uint32_t _appliedSpatialTemporalMask = 0;
    float _appliedLumaRecovery = 1.0f;
    float _appliedChromaRecovery = 1.0f;
    float _appliedFloorHandoverCorrelationMix = -1.0f;
    int _appliedNormalsInViewSpace = -1;

    // The definition of the depth field every view-space position in the chain is built from,
    // as one comparable token: which source produced it, and the convention that reads it.
    // A history accumulated under one definition cannot be reprojected onto another, so a
    // change here has to drop it in the same frame rather than let the two meet.
    //
    // The title's resource pointer is deliberately not part of the token. DLSS titles publish
    // a ring of per-frame buffers, so the pointer moves without the field moving, and a token
    // that followed it would reset every frame; a format or subrect change on the same source
    // is a precision or placement change, not a different field.
    struct DepthDefinition
    {
        bool titleLinearDepth = false; // the title's own linearisation, not the derived field
        bool rightHanded = false;      // the sign convention applied to depth

        bool operator==(const DepthDefinition&) const = default;
    };

    // False until a frame has resolved one, so the first definition is not reported - and
    // acted on - as a change from the zero-initialised default.
    bool _hasAppliedDepthDefinition = false;
    DepthDefinition _appliedDepthDefinition {};

    bool _hasDenoiserHistory = false;
    // SR continuity depends on actual SR dispatches, independently of RR history.
    bool _upscalerResetPending = true;
    // True once this instance has recorded preprocessor work into a command list.
    // Releasing the converter's textures after that point can free resources an
    // already-submitted command list still references, so the automatic
    // signal-classification path must request a feature rebuild instead.
    bool _preprocessorHasRecordedWork = false;
    bool _viewFromStreamline = false;
    bool _projectionFromStreamline = false;
    bool _logNextDenoiserDispatch = true;
    bool _lastDispatchRequestedReset = false;
    uint32_t _lastDenoiserRenderWidth = 0;
    uint32_t _lastDenoiserRenderHeight = 0;
    uint64_t _denoiserDispatchAttempts = 0;
    uint64_t _denoiserDispatchSuccesses = 0;
    uint64_t _denoiserDispatchFailures = 0;


    // Matrices
    // Row-major storage with column-vector multiplication semantics.
    DirectX::XMMATRIX _invViewMatrix;   // Camera rotation and translation
    DirectX::XMMATRIX _viewMatrix;      // World to camera space
    DirectX::XMMATRIX _prevViewMatrix;  // Last world to camera space
    DirectX::XMMATRIX _projMatrix;      // Unjittered perspective projection
    FSRDCamera::Planes _cameraPlanes;
    bool _isRightHanded;                // True if the camera matrix is right handed

    std::unique_ptr<FSRDPreprocessor_Dx12> FSRDConvShader;

    bool InitFSR3(const NVSDK_NGX_Parameter* InParameters) override;

    RRResult CreateDenoiserContext();

    RRResult QueryDenoiserVersions();

    void DestroyDenoiserContext();

    RRResult UpdateSize();

    /**
     * @brief Generates FFX denoiser configuration and input buffers from DLSS-RR inputs and NGX configurations.
     * Converts and repacks resources internally.
     */
    RRResult PrepareDenoiserInput(ID3D12GraphicsCommandList* InCommandList, const NVSDK_NGX_Parameter& ngxParams,
                              ffxDispatchDescDenoiser& dispatchDesc,
                              ffxDispatchDescDenoiserAmbientOcclusion& ambientOcclusion,
                              ffxDispatchDescDenoiserDirectDiffuse& directDiffuse,
                              ffxDispatchDescDenoiserIndirectSpecular& indirectSpecular,
                              ffxDispatchDescUpscale& upscalerDesc, bool isUpscaleBypassed);

    bool AcquireTaggedAmbientOcclusionResources(bool logFailure);
    bool PublishAmbientOcclusionOutput(ID3D12GraphicsCommandList* commandList);

    // What declared D3D12 resource states a Streamline tag may carry when acquired.
    // Every policy shares the same frame/viewport/lifetime validation; only the
    // state requirement differs.
    enum class TagStatePolicy : uint8_t
    {
        // The tag is read as-is, so the declaration must already carry a
        // shader-readable bit.
        RequireShaderRead,
        // Any declared state is accepted; the consumer owns how the resource is used.
        AnyDeclaredState,
        // Shader-readable states plus COMMON. COMMON carries no readable bit but is
        // legal to transition out of: the consumer records the declared-to-read
        // barrier and hands the resource back through that state.
        AllowCommonTransition,
    };

    bool AcquireSLTaggedResource(
        const RRD3D12SignalTagSnapshot& snapshot, RRTaggedSignal signal,
        const char* sourceName, TagStatePolicy statePolicy,
        Microsoft::WRL::ComPtr<ID3D12Resource>& resource,
        RRTaggedResourceDiagnostic& diagnostic);

    /**
     * @brief Retrieves DLSS-RR inputs to populate the inputs for the interop layer in order to generate
     FSR-RR compatible buffers.
     */
    RRResult PrepareDenoiseConvInput(const NVSDK_NGX_Parameter& inParams);

    void ResolveSpecularHitDistance(const NVSDK_NGX_Parameter& inParams,
                                    const RRD3D12SignalTagSnapshot& rrTagSnapshot,
                                    uint32_t renderWidth, uint32_t renderHeight,
                                    uint32_t motionWidth, uint32_t motionHeight,
                                    ID3D12Resource* ngxSpecularHitDistance,
                                    ID3D12Resource* ngxSpecularRayDirectionHitDistance,
                                    const DirectX::XMUINT2& ngxSpecularHitDistanceBase,
                                    const DirectX::XMUINT2& ngxSpecularRayDirectionHitDistanceBase);

    void AcquireOptionalInputs(const NVSDK_NGX_Parameter& inParams,
                              const RRD3D12SignalTagSnapshot& rrTagSnapshot,
                              uint32_t renderWidth, uint32_t renderHeight);

    void ResolveDiffuseHitDistance(const NVSDK_NGX_Parameter& inParams,
                                   uint32_t renderWidth, uint32_t renderHeight);

    bool ResolveCameraMatrices(const NVSDK_NGX_Parameter& inParams,
                               const sl::Constants& slData, bool hasCurrentSLConstants);

    RRResult ResolveSignalTypes(bool isReady);
    void PublishSignalStatus()
    {
        const FSRDSignals::Status status { _signalsObserved, _guides.specular, _guides.diffuse,
                                           _unsupportedAlbedoRecovery, _plan };
        _signalStatus.store(status.Pack(), std::memory_order_relaxed);
    }


    /**
     * @brief Converts previously retrieved DLSS-RR resources into FSR-RR inputs.
     */
    bool ConvertDenoiserBuffers(ID3D12GraphicsCommandList* InCommandList);

    void ApplyRoutingSettings(float biasStrength, float responsivityThreshold, bool responsivityInvert);

    // Decides whether the title's depth is hardware or already linear, and applies it.
    void ApplyDepthInterpretation();

    /**
     * @brief Dispatches FSR-RR denoiser converted inputs. Runs before upscaler.
     */
    RRResult DispatchDenoiser(ID3D12GraphicsCommandList* InCommandList, const ffxDispatchDescDenoiser& dispatchDesc);

    void CommitDenoiserHistory() noexcept;

    // Re-derives the conversion inputs that PrepareDenoiseConvInput froze from the value
    // _hasDenoiserHistory had before a change check invalidated it. Every check that resets
    // history after that point has to call this, or the conversion pass reprojects against a
    // previous-frame depth produced under the setting that was just abandoned, on the very
    // frame the RR dispatch resets.
    void RefreshHistoryDerivedInputs() noexcept;

    void InvalidateDenoiserHistory() noexcept
    {
        if (FSRDConvShader) FSRDConvShader->InvalidateCompositionHistory();
        _upscalerResetPending = true;
        _hasDenoiserHistory = false;
        _lastDispatchRequestedReset = false;
    }

    RRResult SetDefaultConfiguration();

    ffxReturnCode_t SetDefaultConfiguration(FfxApiConfigureDenoiserKey key);

    ffxReturnCode_t ApplyConfiguration(FfxApiConfigureDenoiserKey key);
};
