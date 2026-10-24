#include "pch.h"
#include "MochizukiBackend.h"
#include "InstallStatus.h"
#include "LmxxfRecordingOwner.h"
#include "../NrFrameDiagHost.h"
#include <resource.h>
#include "ProxyDiagnostics.h"
#include "../NrCoverageHost.h"
#include "mochizuki_runtime/MochizukiNrApi.h"
#include "mochizuki_runtime/MochizukiVulkanRegistry.h"
#include "../amd/GraphicsInvocation.h"
#include <State.h>
#include <wrl/client.h>
#include <cmath>

// NrFrameDiag: last Vulkan core GPU ms from the runtime (cheap stats copy; no GPU access).
static PFN_MochizukiNrGetInfo gDiagGetInfo = nullptr;
static float DiagCoreGpuMs(void* context, float* encodeMs, float* transferMs)
{
    if (!gDiagGetInfo || !context) return 0;
    MochizukiNrInfo info {sizeof info};
    if (gDiagGetInfo(context, &info) != LMXXF_NR_OK) return 0.f;
    if (encodeMs) *encodeMs = info.gpu_ms_encode_last;
    if (transferMs) *transferMs = info.gpu_ms_transfer_last;
    return info.gpu_ms_last;
}

namespace DlssNr::Backend
{
namespace
{
// Recording-proxy fallback thresholds (see MochizukiProxyFallbackEnabled).
// ~120 evaluations is about two seconds at 60 fps, long enough that a slow start
// cannot trip it; a 20% miss rate is already visible as flicker.
constexpr uint32_t kProxyFallbackMinEvaluations = 120;
constexpr uint32_t kProxyFallbackMissPercent = 20;

void SubmissionFailure(const LmxxfRecording::SessionOwner::Failure& failure) noexcept
{
    try {
        LOG_WARN("mochizuki submission stopped: phase={} result={} queue={:p}: {}",
                 failure.phase, failure.result, failure.queue, failure.error.data());
    } catch (...) {}
}
void SubmissionTrace(uint64_t frame, const char* phase, const void* object, int32_t result) noexcept
{
    try {
        LOG_INFO("mochizuki submission frame={} phase={} object={:p} result={}", frame, phase, object, result);
    } catch (...) {}
}
struct DirectStateScope
{
    Submission::IRecordingResources* resources = nullptr; // borrowed; QI owner outlives this scope
    ~DirectStateScope() { if (resources) resources->EndDirectState(); }
};
}
struct MochizukiBackend::Impl
{
    Coverage::Tracker coverage {"mochizuki"};
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    std::filesystem::path directory;
    HMODULE module = nullptr;
    LmxxfNrApi api {};
    PFN_MochizukiNrPrepareFrame prepare = nullptr;
    PFN_MochizukiNrGetDirectCommandBuffer getDirectCommandBuffer = nullptr;
    PFN_MochizukiNrRecordDirect recordDirect = nullptr;
    PFN_MochizukiNrSetControls setControls = nullptr;
    PFN_MochizukiNrGetInfo getInfo = nullptr;
    PFN_MochizukiNrGetBuildProgress getBuildProgress = nullptr;
    void (*setLogging)(uint32_t) = nullptr;
    // Optional exposure exports (absent in an older runtime: the lane keeps its manual white point).
    PFN_MochizukiNrSetExposure setExposure = nullptr;
    PFN_MochizukiNrSetAutoExposure setAutoExposure = nullptr;
    PFN_MochizukiNrSetExposureTexture setExposureTexture = nullptr;
    std::shared_ptr<LmxxfRecording::SessionOwner> owner;
    uint64_t frameId = 0, retryAt = 0, infoAt = 0, logAt = 0;
    bool failed = false;
    unsigned errors = 0;
    unsigned consecutivePrepareFailures = 0;
    unsigned tracedRecordings = 0;
    // Recording-proxy coverage. A large share of the game's command lists is never
    // wrapped by SubmissionHooks, so those frames go through as the original colour
    // while the wrapped ones run NR. Alternating between the two is the Forza
    // flicker. Track the ratio and, once it is proven too high, hold NR off for the
    // rest of the session rather than alternate (see MochizukiProxyFallbackEnabled).
    uint32_t proxyEvaluations = 0;   // lists offered this session
    uint32_t proxyMissing = 0;       // of those, without a recording proxy
    bool proxyFallback = false;      // latched: too many misses, NR held off
    const void* lastProxiedList = nullptr; // last list that did carry a proxy
    mutable std::mutex mutex;
    std::string status = "mochizuki: idle";
    NrTimingSnapshot timing {};
    MochizukiNrBuildProgress progress {};
    MochizukiNrInfo details {};

    void Status(const std::string& text) { std::lock_guard lock(mutex); status = text; }
    void Error(const char* phase)
    {
        if (owner) owner->activity.Reset();
        { std::lock_guard lock(mutex); timing = {}; details = {}; }
        char error[256] {};
        if (api.GetLastError) api.GetLastError(error, sizeof error);
        Status(std::string("mochizuki: ") + phase + (error[0] ? ": " : "") + error);
        if (++errors <= 3 || errors % 300 == 0) LOG_WARN("mochizuki {}: {}", phase, error);
    }
    bool Ensure(MochizukiBackend& backend)
    {
        if (owner) return !failed && !owner->failed;
        if (failed || GetTickCount64() < retryAt) return false;
        retryAt = GetTickCount64() + 5000;
        ScopedSkipVulkanHooks skipVk;
        ScopedCreatingD3DDevice creating;
        if (!module)
        {
            module = LoadLibraryExW((directory / L"MochizukiNrRuntime.dll").c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (!module) { Status("mochizuki: runtime could not load; install the complete package and current AMD driver"); return false; }
            auto getApi = reinterpret_cast<PFN_MochizukiNrGetApi>(GetProcAddress(module, "MochizukiNrGetApi"));
            prepare = reinterpret_cast<PFN_MochizukiNrPrepareFrame>(GetProcAddress(module, "MochizukiNrPrepareFrame"));
            getDirectCommandBuffer = reinterpret_cast<PFN_MochizukiNrGetDirectCommandBuffer>(
                GetProcAddress(module, "MochizukiNrGetDirectCommandBuffer"));
            recordDirect = reinterpret_cast<PFN_MochizukiNrRecordDirect>(GetProcAddress(module, "MochizukiNrRecordDirect"));
            setControls = reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(module, "MochizukiNrSetControls"));
            getInfo = reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(module, "MochizukiNrGetInfo"));
            getBuildProgress = reinterpret_cast<PFN_MochizukiNrGetBuildProgress>(GetProcAddress(module, "MochizukiNrGetBuildProgress"));
            setLogging = reinterpret_cast<void (*)(uint32_t)>(GetProcAddress(module, "MochizukiNrSetLogging"));
            setExposure = reinterpret_cast<PFN_MochizukiNrSetExposure>(GetProcAddress(module, "MochizukiNrSetExposure"));
            setAutoExposure = reinterpret_cast<PFN_MochizukiNrSetAutoExposure>(GetProcAddress(module, "MochizukiNrSetAutoExposure"));
            setExposureTexture = reinterpret_cast<PFN_MochizukiNrSetExposureTexture>(GetProcAddress(module, "MochizukiNrSetExposureTexture"));
            api.struct_size = sizeof api;
            if (!getApi || !prepare || !getDirectCommandBuffer || !recordDirect || !setControls || !getInfo || !getBuildProgress || !setLogging ||
                getApi(LMXXF_NR_ABI_VERSION, &api) != LMXXF_NR_OK || !api.Create || !api.Destroy ||
                !api.PrepareSession || !api.RecordInputs || !api.RecordOutputs || !api.EnqueueHip ||
                !api.BeginRecordingExecution || !api.EndRecordingExecution || !api.InvalidateRecording ||
                !api.CollectRecording || !api.GetStatus || !api.GetLastError || !api.ResetHistory)
            {
                // A later NR off/on clears failed. Do not let it reuse an
                // unvalidated function table or missing required exports.
                FreeLibrary(module); module = nullptr; api = {};
                failed = true;
                Status("mochizuki: host/runtime mismatch; overwrite with the complete current package");
                return false;
            }
            // Installed before Create/AdoptVkd3d; the host outlives this runtime.
            if (const auto setQuery = reinterpret_cast<mz::device::SetQuery>(
                    GetProcAddress(module, "MochizukiNrSetVulkanDeviceQuery")))
                setQuery(&mz::device::QueryCreatedDevice);
        }
        setLogging((Config::Instance()->LogToFile.value_or_default() || Config::Instance()->NrFlickerDiag.value_or_default()));
        if (!HasMochizukiSelectedModel(directory, Config::Instance()->MochizukiNetworkModel.value_or_default()))
        { Status("mochizuki: selected model missing; run Setup to install the selected model (see docs/mochizuki.md)"); return false; }
        LmxxfNrCreateInfo ci {sizeof ci, device.Get(), queue.Get(), directory.c_str(),
                            LMXXF_NR_CREATE_FLAG_RECORDING_LEASES | LMXXF_NR_CREATE_FLAG_INLINE_QUEUE};
        void* session = nullptr;
        if (api.Create(&ci, &session) != LMXXF_NR_OK) { Error("Create failed"); return false; }
        MochizukiNrInfo contract {sizeof contract};
        const auto defaults = reinterpret_cast<PFN_MochizukiNrGetControlDefaults>(
            GetProcAddress(module, "MochizukiNrGetControlDefaults"));
        MochizukiNrControls controlContract {sizeof controlContract};
        if (!defaults || defaults(&controlContract) != LMXXF_NR_OK || getInfo(session, &contract) != LMXXF_NR_OK) {
            api.Destroy(session);
            failed = true;
            Status("mochizuki: host/runtime mismatch; overwrite with the complete current package");
            return false;
        }
        owner = LmxxfRecording::SessionOwner::Create(api, session, LmxxfRecording::SessionOwner::kInlineAhead);
        gDiagGetInfo = getInfo;
        if (owner) owner->diagCoreGpuMs = &DiagCoreGpuMs;
        if (!owner) { api.Destroy(session); Status("mochizuki: session ownership allocation failed"); return false; }
        { std::lock_guard lock(mutex); timing = {}; details = {}; backend.AdvanceTimingRuntimeId(); }
        coverage.counters->prepared.store(false, std::memory_order_relaxed);
        owner->coverage = coverage.counters;
        owner->onFailure = SubmissionFailure;
        // ACO is chosen when the Vulkan device is created. Push the current ini before PrepareSession.
        MochizukiNrControls controls {};
        controls.struct_size = sizeof controls;
        [[maybe_unused]] MochizukiNrFrameInfo frame {};
        [[maybe_unused]] bool temporal = true;
#define MZ_F(name, def, low, high, group, label, target) target = Config::Instance()->name.value_or_default();
#define MZ_U MZ_F
#define MZ_B MZ_F
#include "MochizukiOptions.inc"
#undef MZ_F
#undef MZ_U
#undef MZ_B
        frame.passes = Config::Instance()->MochizukiEffectivePasses();
        if (setControls(session, &controls) != LMXXF_NR_OK)
        { Error("controls rejected"); LOG_INFO("mochizuki release_reason=create_failed:controls_rejected model=0 frames=0 last_network_ms=0.00"); owner.reset(); return false; }
        if (api.PrepareSession(session) != LMXXF_NR_OK)
        { Error("Vulkan unavailable"); LOG_INFO("mochizuki release_reason=create_failed:vulkan_unavailable model=0 frames=0 last_network_ms=0.00"); owner.reset(); return false; }
        return true;
    }
    void Info()
    {
        if (!owner) return;
        if (owner->failed.load(std::memory_order_acquire))
        {
            const auto& failure = owner->failure;
            std::lock_guard lock(mutex);
            progress.active = 0; timing = {}; details = {};
            status = std::string("mochizuki: submission failed at ") +
                (failure.phase ? failure.phase : "unknown stage") + ": " + failure.error.data();
            return;
        }
        if (failed) return; // Keep a synchronous Record error until the session is replaced.
        const auto now = GetTickCount64();
        if (now - infoAt < 500) return;
        infoAt = now;
        MochizukiNrBuildProgress build {sizeof build};
        if (getBuildProgress(owner->context, &build) == LMXXF_NR_OK) {
            std::lock_guard lock(mutex); progress = build;
        }
        char text[256] {};
        api.GetStatus(owner->context, text, sizeof text);
        Status(text);
        MochizukiNrInfo info {sizeof info};
        if (getInfo(owner->context, &info) != LMXXF_NR_OK) return;
        NrTimingSnapshot next {};
        next.struct_size = sizeof next; next.version = NR_TIMING_VERSION;
        next.enabled = !info.failed;
        if (info.gpu_samples)
        {
            next.reserved = 1u;
            auto& gpu = next.stages[NR_GPU_NETWORK];
            gpu.samples = info.gpu_samples;
            gpu.last_ms = info.gpu_ms_last;
            gpu.mean_ms = info.gpu_ms_median;
            gpu.max_ms = info.gpu_ms_max;

            gpu.last_tick_ms = info.gpu_tick;
        }
        { std::lock_guard lock(mutex); timing = next; details = info; }
        if (Config::Instance()->NrTimingLog.value_or_default() && now - logAt >= 10000)
        {
            logAt = now;
            LOG_INFO("mochizuki Vulkan core GPU median={:.3f} ms p95={:.3f} ms; means: network={:.3f} other={:.3f} total={:.3f} ms; excludes bridge copies; frames={}; accepted_samples={}; core_certified_harvest_reads={}; core_unavailable_invalid_rejected_reads={}",
                     info.gpu_ms_median, info.gpu_ms_p95, info.gpu_ms_network_mean, info.gpu_ms_other_mean,
                     info.gpu_ms_mean, info.frames, info.gpu_samples, info.recording_timing_reads,
                     info.recording_timing_rejected);
        }
    }
};

MochizukiBackend::MochizukiBackend(ID3D12Device* d, ID3D12CommandQueue* q, const std::filesystem::path& dir)
    : p(std::make_unique<Impl>()) { p->device = d; p->queue = q; p->directory = dir; }
MochizukiBackend::~MochizukiBackend() { Shutdown(); }

ID3D12Resource* MochizukiBackend::Record(ID3D12GraphicsCommandList* cmd, const AmdPreSr::Frame& input,
                                       const AmdPreSr::Settings& settings)
{
    return RecordLayers(cmd, input, settings, nullptr);
}

ID3D12Resource* MochizukiBackend::RecordLayers(ID3D12GraphicsCommandList* cmd, const AmdPreSr::Frame& incoming,
                                              const AmdPreSr::Settings&, ID3D12Resource** first)
{
    auto input = incoming;
    if (first) *first = nullptr;
    std::lock_guard lifetime(Submission::RecordingMutex());
    Coverage::Attempt coverage(p->coverage, cmd);
    LmxxfRecording::Collect();
    // Callback failures and background progress must remain visible even when
    // this frame is rejected before PrepareFrame/Record.
    p->Info();
    // Latched proxy fallback: the miss rate proved the lists are unevenly wrapped, so
    // NR would alternate with the original colour. Stay off rather than flicker.
    if (p->proxyFallback) { coverage.Set(Coverage::Outcome::HeldOff, "proxy_fallback_latched"); return nullptr; }
    if (!cmd || !input.colour) { coverage.Set(Coverage::Outcome::InvalidInput, "cmd_or_colour_missing"); return nullptr; }
    Microsoft::WRL::ComPtr<Submission::ILogicalCommandList> logical;
    Microsoft::WRL::ComPtr<Submission::IRecordingResources> resources;
    // Count every offered list, so coverage={success}/{offered} is the real ratio and
    // not an artefact of counting only the failures (which always reads as 100%).
    const uint32_t offered = ++p->proxyEvaluations;
    const HRESULT proxyHr = cmd->QueryInterface(IID_PPV_ARGS(&logical));
    const HRESULT resourcesHr = cmd->QueryInterface(IID_PPV_ARGS(&resources));
    if (FAILED(proxyHr) || FAILED(resourcesHr))
    {
        coverage.Set(Coverage::Outcome::ProxyMissing, "logical_or_resources_qi_failed");
        const uint32_t missing = ++p->proxyMissing;
        p->Status("mochizuki: recording proxy unavailable; original frame");
        if (missing <= 5 || missing % 1000 == 0)
        {
            char origin[MAX_PATH * 2] {};
            ProxyDiagnostics::DescribeList(cmd, origin, sizeof origin);
            LOG_WARN("mochizuki missing recording proxy: frame={} list={:p} type={} logicalQi={:08X} resourcesQi={:08X} sample={} coverage={}/{} ({}% miss) lastProxy={:p} device={:p} {}",
                     p->frameId, static_cast<void*>(cmd), unsigned(cmd->GetType()), unsigned(proxyHr), unsigned(resourcesHr),
                     missing, offered - missing, offered, missing * 100 / (offered ? offered : 1),
                     p->lastProxiedList, static_cast<void*>(p->device.Get()), origin);
        }
        // Half the lists unwrapped means NR and the original colour alternate frame by
        // frame, which is exactly the flicker. Rather than keep flipping, stop NR for
        // this session once the miss rate is clearly too high. Opt-in only.
        if (!p->proxyFallback && Config::Instance()->MochizukiProxyFallbackEnabled.value_or_default() &&
            offered >= kProxyFallbackMinEvaluations &&
            missing * 100 >= offered * kProxyFallbackMissPercent)
        {
            p->proxyFallback = true;
            p->Status("mochizuki: recording proxy coverage too low; NR held off to stop flicker (restart to try again)");
            LOG_WARN("mochizuki recording proxy coverage {}/{} ({}%) below {}%; holding NR off for the rest of the session to avoid flicker",
                     missing, offered, missing * 100 / (offered ? offered : 1), kProxyFallbackMissPercent);
        }
        return nullptr;
    }
    p->lastProxiedList = cmd;
    const auto coverageId = logical->Identity();
    coverage.Identity(coverageId.list, coverageId.generation);
    // The frame id advances only for frames we can actually record: a rejected frame
    // must not consume a number, or the next accepted frame looks like a gap and the
    // runtime resets its temporal history for nothing.
    const auto frameId = ++p->frameId;
    if (!p->Ensure(*this)) { coverage.Set(Coverage::Outcome::SessionUnavailable, "ensure_session"); return nullptr; }
    // Resolve the current generation, not the initial producer or a queried native alias.
    // An adopted device missing its extension must fail, never fall back to a side queue.
    auto* nativeList = logical->RecordingNativeList();
    uint64_t commandBuffer = 0;
    if (p->getDirectCommandBuffer(p->owner->context, nativeList, &commandBuffer) != LMXXF_NR_OK) {
        coverage.Set(Coverage::Outcome::RecordFailed, "direct_command_buffer_unavailable");
        p->Error("direct command buffer unavailable");
        return nullptr;
    }
    const bool directRecording = commandBuffer != 0;
    const bool inlineMode = !directRecording && Config::Instance()->NrInline.value_or_default();
    if (!directRecording && logical->IsSplitIneligible()) logical->TraceOpenQueries();
    InlineRecording::StateScope inlineState;
    DirectStateScope directState;
    if (!directRecording && !inlineMode && logical->IsSplitIneligible()) {
        const char* reason = logical->SplitRejectionReason();
        coverage.Set(Coverage::Outcome::Rejected, reason);
        p->Status(std::string("mochizuki: recording boundary unavailable: ") + reason + "; original frame");
        static unsigned rejected = 0;
        if (++rejected <= 5 || rejected % 1000 == 0) {
            const auto id = logical->Identity();
            LOG_WARN("mochizuki recording rejected: frame={} list={:p} listId={} generation={} reason={}",
                     frameId, static_cast<void*>(cmd), id.list, id.generation, reason);
        }
        return nullptr;
    }
    const bool motion = (input.nativeGuideScale || !input.motionJittered) &&
        Config::Instance()->MochizukiTemporal.value_or_default();
    if (directRecording) {
        ID3D12Resource* sources[] {input.colour, motion ? input.motion : nullptr, input.depth, input.exposure};
        D3D12_RESOURCE_STATES states[] {input.colourState, input.motionState, input.depthState, input.exposureState};
        if (FAILED(resources->BeginDirectState(4, sources, states))) {
            coverage.Set(Coverage::Outcome::InputRejected, resources->InlineRejectionReason());
            p->Status(std::string("mochizuki: direct input state unavailable: ") + resources->InlineRejectionReason());
            return nullptr;
        }
        directState.resources = resources.Get();
        input.colourState = states[0]; input.motionState = states[1];
        input.depthState = states[2]; input.exposureState = states[3];
        // Native layout normalization may rotate vkd3d's command buffer. Selection
        // used the earlier query; the recording must use this current handle.
        if (p->getDirectCommandBuffer(p->owner->context, nativeList, &commandBuffer) != LMXXF_NR_OK || !commandBuffer) {
            coverage.Set(Coverage::Outcome::RecordFailed, "direct_command_buffer_changed");
            p->Error("direct command buffer unavailable after input normalization");
            return nullptr;
        }
    }
    if (!directRecording && FAILED(inlineState.Begin(cmd, input, motion, false, false, !inlineMode))) {
        coverage.Set(Coverage::Outcome::InputRejected, inlineState.Reason().c_str());
        p->Status(std::string("mochizuki: input state unavailable: ") + inlineState.Reason());
        static unsigned rejectedInput = 0;
        if (++rejectedInput <= 5 || rejectedInput % 1000 == 0)
            inlineState.TraceFailure("mochizuki", frameId, cmd, input, motion, false, false);
        return nullptr;
    }
    if (!directRecording && p->owner->InlineBusy()) {
        coverage.Set(Coverage::Outcome::Busy, "inline_ahead_cap");
        p->owner->InlineBypassed();
        p->Status("mochizuki: inline ahead cap reached; original frame");
        FrameDiag::EmitBypass("mochizuki", frameId, unsigned(p->owner->inlineInFlight.size()),
                              incoming.width, incoming.height, OPTI_VERSION);
        return nullptr;
    }
    auto* cfg = Config::Instance();
    p->setLogging((cfg->LogToFile.value_or_default() || cfg->NrFlickerDiag.value_or_default()));
    MochizukiNrFrameInfo frame {};
    MochizukiNrControls controls {};
    controls.struct_size = sizeof controls;
    bool temporal = true;
#define MZ_F(name, def, low, high, group, label, target) target = cfg->name.value_or_default();
#define MZ_U MZ_F
#define MZ_B MZ_F
#include "MochizukiOptions.inc"
#undef MZ_F
#undef MZ_U
#undef MZ_B
    frame.passes = cfg->MochizukiEffectivePasses();
    frame.struct_size = sizeof frame;
    frame.frame_id = frameId;
    frame.flags = LMXXF_NR_FRAME_FLAG_STRENGTH | (temporal ? MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL : 0) |
        (first ? LMXXF_NR_FRAME_FLAG_FIRST_PASS : 0) |
        (inlineMode ? MOCHIZUKI_NR_FRAME_FLAG_INLINE_EXECUTION : 0) |
        (directRecording ? MOCHIZUKI_NR_FRAME_FLAG_DIRECT_RECORDING : 0) |
        (input.nativeGuideScale ? MOCHIZUKI_NR_FRAME_FLAG_AFTER_UPSCALE : 0) |
        (FrameDiag::Enabled() ? MOCHIZUKI_NR_FRAME_FLAG_STAGE_TIMING : 0) |
        (inlineMode && Config::Instance()->NrInlineComputeConvert.value_or_default() ? MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_CONVERT : 0) |
        // Inline only: split keeps its blits, motion and network (switching modes may rebuild once).
        (inlineMode && Config::Instance()->NrInlineComputeMotion.value_or_default() ? MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_MOTION : 0) |
        (inlineMode && Config::Instance()->NrInlineComputeDownscale.value_or_default() ? MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_DOWNSCALE : 0) |
        (inlineMode && Config::Instance()->NrInlineComputeExport.value_or_default() ? MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_EXPORT : 0);
    const auto desc = input.colour->GetDesc();
    frame.color = input.colour;
    frame.color_width = input.width && input.width <= desc.Width ? input.width : UINT(desc.Width);
    frame.color_height = input.height && input.height <= desc.Height ? input.height : desc.Height;
    frame.color_state = input.colourState;
    // Host-resampled SR -> NR has no jitter contract, so those vectors are dropped.
    // Mochizuki's own post-SR path passes the game vectors through, as the runtime does.
    frame.motion = input.motionJittered && !input.nativeGuideScale ? nullptr : input.motion;
    frame.motion_state = input.motionState;
    frame.motion_width = input.motionWidth;
    frame.motion_height = input.motionHeight;
    frame.motion_scale_x = input.motionScaleX;
    frame.motion_scale_y = input.motionScaleY;
    frame.reset = input.reset;
    frame.depth = input.depth;
    frame.depth_state = input.depthState;
    if (input.depth) {
        const auto depthDesc = input.depth->GetDesc();
        frame.depth_width = UINT(depthDesc.Width);
        frame.depth_height = depthDesc.Height;
    }
    frame.depth_inverted = input.depthInverted ? 1u : 0u;
    if (p->setControls(p->owner->context, &controls) != LMXXF_NR_OK) { p->Error("controls rejected"); return nullptr; }
    // Exposure for the linear encode, same semantics as the lmxxf lane (native_codec_encode.hlsl EffectivePaperWhite):
    // a usable game exposure texture gives exposure = texel * DLSS_Exposure_Scale / DLSS_Pre_Exposure (read back by
    // the runtime one frame late); without one, a host pre-exposure != 1 gives exposure = 1 / pre-exposure. The
    // runtime divides the white point by it. MochizukiWhitePointSource picks the sources: 0 none (manual white point),
    // 1 game only, 2 the runtime's own meter only (the game's exposure is not handed over, so no texel readback),
    // 3 both (the runtime prefers the game's and runs the meter only for a frame with neither). Modes 0 and 1 keep
    // the meter off, so it records nothing.
    {
        const uint32_t source = cfg->MochizukiWhitePointSource.value_or_default();
        const bool reno = cfg->MochizukiRenoDX.value_or_default();
        // RenoDX has its own feed/meter settings. Native white-point preferences
        // remain stored and apply again when RenoDX is disabled.
        const bool useGame = reno ? cfg->MochizukiRenoFeedMode.value_or_default() != 1u :
                                   source == 1u || source == 3u;
        const bool useMeter = !reno && (source == 2u || source == 3u);
        void* texture = useGame ? static_cast<void*>(input.exposure) : nullptr;
        const float pre = std::isfinite(input.preExposure) && input.preExposure > 0.f ? input.preExposure : 1.f;
        const float scale = std::isfinite(input.exposureScale) && input.exposureScale > 0.f ? input.exposureScale : 1.f;
        if (p->setExposureTexture)
            p->setExposureTexture(p->owner->context, texture, uint32_t(input.exposureState), pre, scale);
        else
            texture = nullptr;
        if (p->setExposure)
            p->setExposure(p->owner->context,
                           useGame && !texture && std::fabs(pre - 1.f) > 1e-3f ? 1.f / pre : 0.f);
        if (p->setAutoExposure)
            p->setAutoExposure(p->owner->context, useMeter ? 1u : 0u);
    }
    // Circuit breaker tripped: stop calling prepare entirely (each failed attempt can leak;
    // seen on Proton at ~128/s until OOM). Reset by the coverage coverage Reset()/activity path
    // on a settings change or NR toggle.
    if (p->consecutivePrepareFailures >= 50)
    {
        p->owner->activity.Reset();
        coverage.Set(Coverage::Outcome::PrepareFailed, "circuit breaker open (50+ consecutive failures)");
        return nullptr;
    }
    LmxxfNrJob job {sizeof job};
    if (p->prepare(p->owner->context, &frame, &job) != LMXXF_NR_OK)
    {
        p->owner->activity.Reset();
        char reason[256] {};
        p->api.GetLastError(reason, sizeof reason);
        p->Info();
        coverage.Set(std::strstr(reason, "building the network") || std::strstr(reason, "input resolution is settling")
            ? Coverage::Outcome::BuildWait : Coverage::Outcome::PrepareFailed, reason);
        if (std::strstr(reason, "building the network"))
            p->Status("mochizuki: building the network; first use may take minutes (original frame)");
        else if (std::strstr(reason, "input resolution is settling"))
            p->Status("mochizuki: waiting for stable input resolution (original frame)");
        else
        {
            p->Status(std::string("mochizuki: ") + reason);
            if (++p->errors <= 3 || p->errors % 6000 == 0) LOG_WARN("mochizuki PrepareFrame: {}", reason);
            ++p->consecutivePrepareFailures;
            if (p->consecutivePrepareFailures == 50)
                LOG_ERROR("mochizuki: {} consecutive prepare failures ({}); circuit breaker opens on the next frame",
                          p->consecutivePrepareFailures, reason);
        }
        return nullptr;
    }
    coverage.Prepared();
    p->consecutivePrepareFailures = 0;
    std::shared_ptr<InlineRecording::Resources> inlineResources;
    if (inlineMode) {
        try {
            const auto* c = Config::Instance();
            const bool reuse = c->NrInlineReuse.value_or_default();
            if (!reuse) p->owner->inlinePool.clear();
            inlineResources = InlineRecording::AcquireResources(reuse ? &p->owner->inlinePool : nullptr, p->device.Get(), input.colour,
                static_cast<ID3D12Resource*>(job.private_output), static_cast<ID3D12Resource*>(job.first_pass_output),
                frame.color_width, frame.color_height, p->owner->inlinePipelines);
            p->owner->inlinePipelines = inlineResources->PipelineCache();
            inlineResources->captureBackoff = c->NrCaptureBackoff.value_or_default();
            inlineResources->gpuArmed = c->NrGpuHandoff.value_or_default();
            const bool armed = inlineResources->gpuArmed;
            const int wsl = c->NrWatchSlices.value_or_default(), wsd = c->NrWatchStartDelay.value_or_default();
            inlineResources->watchSlices = wsl == 256 || wsl == 1024 ? UINT(wsl) : 0u;
            inlineResources->watchStartDelay = wsd >= 1 && wsd <= 8 ? UINT(wsd) : 0u;
            inlineResources->adaptiveBudget = armed && c->NrAdaptiveWaitBudget.value_or_default();

            InlineAutoSpin::RuntimeConfigInput autoCfg {};
            autoCfg.inlineEnabled = true;
            autoCfg.isMochizuki = true;
            autoCfg.gpuHandoff = armed;
            autoCfg.autoSpinEnabled = c->NrInlineAutoSpin.value_or_default();
            autoCfg.adaptiveWaitBudget = inlineResources->adaptiveBudget;
            if (c->NrSpinPollDelay.has_value()) autoCfg.fixedPollDelay = c->NrSpinPollDelay.value();
            if (c->NrSpinSlices.has_value()) autoCfg.fixedSlices = c->NrSpinSlices.value();

            InlineAutoSpin::WorkloadSignature workloadSig {};
            workloadSig.sessionId = reinterpret_cast<uintptr_t>(p->owner.get());
            // The record queue is not the queue that executes this frame. 0 stays
            // unknown until BeforeExecute latches the real one. The present ring
            // is still inactive on this call, so a zero swapchain is also unknown.
            workloadSig.gameQueueId = reinterpret_cast<uintptr_t>(p->owner->lastInlineGameQueue);
            {
                const auto& ring = FrameDiag::AutoSpinPresentRing();
                workloadSig.swapchainId = ring.CurrentSerial() > 0 ? ring.CurrentSwapchainId() : 0;
            }
            workloadSig.width = frame.color_width;
            workloadSig.height = frame.color_height;
            workloadSig.format = static_cast<uint32_t>(desc.Format);
            workloadSig.passes = static_cast<int32_t>(frame.passes);
            workloadSig.networkFlags = frame.flags & ~MOCHIZUKI_NR_FRAME_FLAG_STAGE_TIMING;
            workloadSig.inlineReuse = reuse;
            workloadSig.watchSlices = static_cast<int>(inlineResources->watchSlices);
            workloadSig.watchStartDelay = static_cast<int>(inlineResources->watchStartDelay);

            p->owner->autoSpin.UpdateConfigAndWorkload(autoCfg, workloadSig);
            FrameDiag::AutoSpinPresentRing().SetActive(p->owner->autoSpin.ShouldMeasure());

            const auto cand = p->owner->autoSpin.CurrentCandidate();
            const int pd = armed ? cand.pollDelay : 0;
            const int slA = armed ? cand.SlicesIniValue() : 0;
            inlineResources->spinPollDelay = pd == 16 || pd == 64 ? UINT(pd) : 0u;
            inlineResources->spinSlices = slA == 256 || slA == 1024 ? UINT(slA) : 0u;
            inlineResources->measureActive = armed && p->owner->autoSpin.ShouldMeasure();
            inlineResources->measureGpuStamps = armed && p->owner->autoSpin.ShouldMeasureGpuStamps();
            inlineResources->autoSpinEpoch = p->owner->autoSpin.CurrentEpoch();

            FrameDiag::Begin(inlineResources->diag, "mochizuki", frameId, frame.color_width, frame.color_height, OPTI_VERSION);
            inlineResources->diag.spinPollDelay = inlineResources->spinPollDelay; inlineResources->diag.spinSlices = inlineResources->spinSlices;
            inlineResources->diag.watchSlices = inlineResources->watchSlices; inlineResources->diag.watchStartDelay = inlineResources->watchStartDelay;
            inlineResources->diag.adaptiveBudget = inlineResources->adaptiveBudget;
            inlineResources->diag.inlineReuse = inlineResources->reused;
            const auto id0 = logical->Identity();
            inlineResources->diag.listId = id0.list;
            inlineResources->diag.generation = id0.generation;
        } catch (const std::exception& error) {
            p->api.InvalidateRecording(p->owner->context, job.handle); p->api.CollectRecording(p->owner->context, job.handle);
            p->Status(std::string("mochizuki: inline unavailable: ") + error.what()); return nullptr;
        }
    } else {
        InlineAutoSpin::RuntimeConfigInput autoCfg {};
        autoCfg.inlineEnabled = false;
        autoCfg.isMochizuki = true;
        p->owner->autoSpin.UpdateConfigAndWorkload(autoCfg, {});
        FrameDiag::AutoSpinPresentRing().SetActive(false);
    }
    auto lease = LmxxfRecording::Attach(p->owner, job.handle, logical.Get(), inlineResources, directRecording);
    if (!lease) { p->Status("mochizuki: this recording already owns NR work"); return nullptr; }
    if (!inlineMode) {
        FrameDiag::Begin(lease->diag, "mochizuki", frameId, frame.color_width, frame.color_height,
                         OPTI_VERSION, directRecording ? "direct" : "split");
        const auto id0 = logical->Identity();
        lease->diag.listId = id0.list;
        lease->diag.generation = id0.generation;
    }
    lease->neural = true;
    if (p->tracedRecordings < 2)
    {
        ++p->tracedRecordings;
        lease->traceId = frameId; lease->trace = SubmissionTrace;
        SubmissionTrace(frameId, "record.begin", cmd, 0);
    }
    auto* invocation = AmdPreSr::GraphicsSnap::GraphicsInvocationFor(reinterpret_cast<uint64_t>(cmd));
    if (invocation) { invocation->commandsRecorded = true; invocation->outcome = "recording_attempted"; }
    if (directRecording) {
        // The runtime owns the native barriers, layout restoration and cache invalidation.
        // Do not restore a private binding scope over its null setters / meta dispatch.
        const int32_t rc = p->recordDirect(p->owner->context, job.handle, nativeList, commandBuffer);
        resources->InvalidateDirectComputeBindings();
        if (rc != LMXXF_NR_OK) {
            // Attach already owns this recipe. Partial native commands cannot be
            // rolled back: keep their resources through submit/tail or list Reset.
            // ready stays false, so this execution is never certified as NR output.
            coverage.Set(Coverage::Outcome::RecordFailed, "direct_recording_failed");
            if (rc == LMXXF_NR_DEVICE_LOST) p->failed = true;
            p->Error("direct recording failed");
            if (invocation) invocation->outcome = "direct_recording_failed";
            return nullptr;
        }
        lease->ready = true;
        coverage.Set(Coverage::Outcome::Recorded, "direct_recording_ready");
        if (lease->trace) SubmissionTrace(frameId, "record.direct.ready", nativeList, rc);
        if (invocation) invocation->outcome = "direct_recorded";
        p->Info();
        if (first) *first = static_cast<ID3D12Resource*>(job.first_pass_output);
        return static_cast<ID3D12Resource*>(job.private_output);
    }
    // NrFrameDiag GPU stamps (default off): capture begin/end, resume, done on the game queue.
    if (inlineResources) inlineResources->DiagStamp(cmd, FrameDiag::kGpuCapBegin);
    else if (lease->diag.active) {
        if (auto stamps = p->owner->AcquireSplitStamps(p->device.Get()))
        { lease->splitStamps = stamps; stamps->Stamp(cmd, FrameDiag::kGpuCapBegin); }
    }
    // With no game exposure texture the lane runs its own exposure meter: a compute dispatch with its own descriptor
    // heap, root signature and root arguments. A split recording tracks those as the game's bindings and seeds the
    // continuation with them, so keep them in a private scope for exactly this call. The scope is unconditional: it is
    // two bookkeeping calls, and which side metered must not decide whether the bindings stay out of the game snapshot.
    resources->BeginPrivateBindings();
    // NrGpuHandoff: the input copies run only if the game claims the gate before the side watch closes it.
    if (inlineResources) inlineResources->BeginInputs(cmd);
    const int32_t inputs = p->api.RecordInputs(p->owner->context, job.handle, cmd);
    resources->EndPrivateBindings();
    if (inputs != LMXXF_NR_OK)
    { p->failed = true; p->Error("input recording failed"); return nullptr; }
    if (inlineResources) { inlineResources->RecordCapture(cmd); inlineResources->RecordWait(cmd); }
    else if (lease->splitStamps) lease->splitStamps->Stamp(cmd, FrameDiag::kGpuCapEnd);
    if (!inlineResources && LmxxfCut::TrySplitAtEvaluate(cmd) != S_OK)
    { p->failed = true; p->Error("recording failed"); return nullptr; }
    if (!inlineResources && lease->splitStamps) lease->splitStamps->Stamp(cmd, FrameDiag::kGpuResume);
    if (p->api.RecordOutputs(p->owner->context, job.handle, cmd) != LMXXF_NR_OK)
    { p->failed = true; p->Error("recording failed"); return nullptr; }
    if (!inlineResources && lease->splitStamps) {
        lease->splitStamps->Stamp(cmd, FrameDiag::kGpuDone); lease->splitStamps->Resolve(cmd);
    }
    if (inlineResources) inlineResources->RecordSelect(cmd, input.colourState);
    lease->ready = true;
    coverage.Set(Coverage::Outcome::Recorded, "recording_ready");
    if (lease->trace) {
        SubmissionTrace(frameId, "record.ready", cmd, 0);
        const auto id = logical->Identity();
        LOG_INFO("mochizuki recording ready: frame={} list={:p} listId={} generation={}",
                 frameId, static_cast<void*>(cmd), id.list, id.generation);
    }
    if (invocation) invocation->outcome = "recorded";
    p->Info();
    if (first) *first = inlineResources ? inlineResources->FirstTexture() : static_cast<ID3D12Resource*>(job.first_pass_output);
    return inlineResources ? inlineResources->OutputTexture() : static_cast<ID3D12Resource*>(job.private_output);
}
void MochizukiBackend::Submitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) { LmxxfRecording::Collect(); }
bool MochizukiBackend::PollRelease()
{
    // AmdBridge::PollReleases walks the shared registry once for every backend.
    return true;
}
void MochizukiBackend::ReleaseSession(const char* reason)
{
    std::lock_guard lifetime(Submission::RecordingMutex());
    // One line per release, before the cached details go: the tester-facing answer
    // to "the network destroyed itself and nothing said why".
    if (p->owner)
    {
        const auto& d = p->details;
        const unsigned model = d.frame_w ? d.network_effective : d.network_requested;
        LOG_INFO("mochizuki release_reason={} model={} frames={} last_network_ms={:.2f}",
                 reason && *reason ? reason : "unspecified", model,
                 static_cast<unsigned long long>(d.frames),
                 double(d.gpu_samples ? (d.gpu_ms_network_last > 0.f ? d.gpu_ms_network_last : d.gpu_ms_last) : 0.f));
    }
    FrameDiag::AutoSpinPresentRing().SetActive(false);
    p->coverage.counters->prepared.store(false, std::memory_order_relaxed);
    p->owner.reset(); p->failed = false; p->retryAt = p->infoAt = 0;
    p->tracedRecordings = 0;
    // Reopening NR or switching back to this backend must get a fresh start, not a stale
    // circuit-breaker trip from a previous session.
    p->consecutivePrepareFailures = 0;
    LmxxfRecording::Collect();
    p->coverage.Flush(true);
    { std::lock_guard lock(p->mutex); p->timing = {}; p->progress = {}; p->details = {}; AdvanceTimingRuntimeId(); p->status = "mochizuki: NR off"; }
    if (!Config::Instance()->NrConvenience.value_or_default() && p->module)
    { FreeLibrary(p->module); p->module = nullptr; p->api = {}; }
}
bool MochizukiBackend::Shutdown()
{
    ReleaseSession("shutdown");
    if (p->module) { FreeLibrary(p->module); p->module = nullptr; p->api = {}; }
    InlineRecording::StopAbortWatchdog(); // owned thread joined here, not at static destruction
    return true;
}
void MochizukiBackend::InvalidateHistory()
{
    std::lock_guard lifetime(Submission::RecordingMutex());
    if (p->owner) p->api.ResetHistory(p->owner->context);
}
std::string MochizukiBackend::Status() const { std::lock_guard lock(p->mutex); return p->status; }
bool MochizukiBackend::IsRunning() const
{
    std::lock_guard lifetime(Submission::RecordingMutex());
    return p->owner && !p->failed && !p->owner->failed.load(std::memory_order_acquire) &&
           p->owner->activity.IsRunning();
}
NrTimingSnapshot MochizukiBackend::Timing() const { std::lock_guard lock(p->mutex); return p->timing; }
MochizukiNrInfo MochizukiBackend::TimingDetails() const { std::lock_guard lock(p->mutex); return p->details; }
MochizukiNrBuildProgress MochizukiBackend::BuildProgress() const { std::lock_guard lock(p->mutex); return p->progress; }
InlineAutoSpin::StatusSnapshot MochizukiBackend::InlineAutoSpinStatus() const
{
    std::lock_guard lifetime(Submission::RecordingMutex());
    const auto* c = Config::Instance();
    InlineAutoSpin::RuntimeConfigInput autoCfg {};
    autoCfg.inlineEnabled = c->NrInline.value_or_default();
    autoCfg.isMochizuki = true;
    autoCfg.gpuHandoff = c->NrGpuHandoff.value_or_default();
    autoCfg.autoSpinEnabled = c->NrInlineAutoSpin.value_or_default();
    autoCfg.adaptiveWaitBudget = autoCfg.gpuHandoff && c->NrAdaptiveWaitBudget.value_or_default();
    if (c->NrSpinPollDelay.has_value()) autoCfg.fixedPollDelay = c->NrSpinPollDelay.value();
    if (c->NrSpinSlices.has_value()) autoCfg.fixedSlices = c->NrSpinSlices.value();

    if (p->owner)
    {
        // Sync config changes immediately so the menu reflects live checkbox/combo edits even if paused.
        p->owner->autoSpin.UpdateConfigAndWorkload(autoCfg, {});
        FrameDiag::AutoSpinPresentRing().SetActive(p->owner->autoSpin.ShouldMeasure());
        return p->owner->autoSpin.GetStatus();
    }
    InlineAutoSpin::Controller temp {};
    temp.UpdateConfigAndWorkload(autoCfg, {});
    return temp.GetStatus();
}
void MochizukiBackend::RequestInlineAutoSpinReeval()
{
    std::lock_guard lifetime(Submission::RecordingMutex());
    if (p->owner)
    {
        p->owner->autoSpin.RequestReevaluate();
        FrameDiag::AutoSpinPresentRing().SetActive(p->owner->autoSpin.ShouldMeasure());
    }
}
}
