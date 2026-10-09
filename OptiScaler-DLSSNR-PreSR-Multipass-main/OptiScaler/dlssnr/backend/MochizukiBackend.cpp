#include "pch.h"
#include "MochizukiBackend.h"
#include "LmxxfRecordingOwner.h"
#include "mochizuki_runtime/MochizukiNrApi.h"
#include "../amd/GraphicsInvocation.h"
#include <State.h>
#include <wrl/client.h>

namespace DlssNr::Backend
{
namespace
{
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
}
struct MochizukiBackend::Impl
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    std::filesystem::path directory;
    HMODULE module = nullptr;
    LmxxfNrApi api {};
    PFN_MochizukiNrPrepareFrame prepare = nullptr;
    PFN_MochizukiNrSetControls setControls = nullptr;
    PFN_MochizukiNrGetInfo getInfo = nullptr;
    PFN_MochizukiNrGetBuildProgress getBuildProgress = nullptr;
    void (*setLogging)(uint32_t) = nullptr;
    std::shared_ptr<LmxxfRecording::SessionOwner> owner;
    uint64_t frameId = 0, retryAt = 0, infoAt = 0, logAt = 0;
    bool failed = false;
    unsigned errors = 0;
    unsigned tracedRecordings = 0;
    mutable std::mutex mutex;
    std::string status = "mochizuki: idle";
    NrTimingSnapshot timing {};
    MochizukiNrBuildProgress progress {};
    MochizukiNrInfo details {};

    void Status(const std::string& text) { std::lock_guard lock(mutex); status = text; }
    void Error(const char* phase)
    {
        if (owner) owner->activity.Reset();
        char error[256] {};
        if (api.GetLastError) api.GetLastError(error, sizeof error);
        Status(std::string("mochizuki: ") + phase + (error[0] ? ": " : "") + error);
        if (++errors <= 3 || errors % 300 == 0) LOG_WARN("mochizuki {}: {}", phase, error);
    }
    bool Ensure()
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
            setControls = reinterpret_cast<PFN_MochizukiNrSetControls>(GetProcAddress(module, "MochizukiNrSetControls"));
            getInfo = reinterpret_cast<PFN_MochizukiNrGetInfo>(GetProcAddress(module, "MochizukiNrGetInfo"));
            getBuildProgress = reinterpret_cast<PFN_MochizukiNrGetBuildProgress>(GetProcAddress(module, "MochizukiNrGetBuildProgress"));
            setLogging = reinterpret_cast<void (*)(uint32_t)>(GetProcAddress(module, "MochizukiNrSetLogging"));
            api.struct_size = sizeof api;
            if (!getApi || !prepare || !setControls || !getInfo || !getBuildProgress || !setLogging ||
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
        }
        setLogging(Config::Instance()->LogToFile.value_or_default());
        if (!std::filesystem::is_regular_file(directory / L"dlssnr-amd/dlssnr.bin"))
        { Status("mochizuki: model missing; run the Mochizuki model installer (see docs/mochizuki.md)"); return false; }
        LmxxfNrCreateInfo ci {sizeof ci, device.Get(), queue.Get(), directory.c_str(), LMXXF_NR_CREATE_FLAG_RECORDING_LEASES};
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
        owner = LmxxfRecording::SessionOwner::Create(api, session);
        if (!owner) { api.Destroy(session); Status("mochizuki: session ownership allocation failed"); return false; }
        owner->onFailure = SubmissionFailure;
        if (api.PrepareSession(session) != LMXXF_NR_OK)
        { Error("Vulkan unavailable"); owner.reset(); return false; }
        return true;
    }
    void Info()
    {
        if (!owner) return;
        if (owner->failed.load(std::memory_order_acquire))
        {
            const auto& failure = owner->failure;
            std::lock_guard lock(mutex);
            progress.active = 0; timing = {};
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
        if (info.gpu_ms_median > 0)
        {
            next.enabled = true;
            next.reserved = 1u;
            auto& gpu = next.stages[NR_GPU_NETWORK];
            gpu.samples = info.gpu_samples ? info.gpu_samples : (info.frames > 0 ? info.frames : 10);
            gpu.last_ms = info.gpu_ms_last > 0 ? info.gpu_ms_last : info.gpu_ms_median;
            gpu.mean_ms = info.gpu_ms_median;
            gpu.max_ms = info.gpu_ms_p95 > 0 ? info.gpu_ms_p95 : info.gpu_ms_median;
            gpu.frame_id = info.frames;
            gpu.last_tick_ms = now;
        }
        { std::lock_guard lock(mutex); timing = next; details = info; }
        if (Config::Instance()->NrTimingLog.value_or_default() && now - logAt >= 10000)
        {
            logAt = now;
            LOG_INFO("mochizuki Vulkan core GPU median={:.3f} ms p95={:.3f} ms; means: network={:.3f} other={:.3f} total={:.3f} ms; excludes bridge copies; frames={}",
                     info.gpu_ms_median, info.gpu_ms_p95, info.gpu_ms_network_mean, info.gpu_ms_other_mean,
                     info.gpu_ms_mean, info.frames);
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

ID3D12Resource* MochizukiBackend::RecordLayers(ID3D12GraphicsCommandList* cmd, const AmdPreSr::Frame& input,
                                              const AmdPreSr::Settings&, ID3D12Resource** first)
{
    if (first) *first = nullptr;
    std::lock_guard lifetime(Submission::RecordingMutex());
    LmxxfRecording::Collect();
    // Callback failures and background progress must remain visible even when
    // this frame is rejected before PrepareFrame/Record.
    p->Info();
    // Include frames rejected before PrepareFrame (for example an active render
    // pass), so the next accepted frame cannot consume history across that gap.
    const auto frameId = ++p->frameId;
    if (!cmd || !input.colour) return nullptr;
    Microsoft::WRL::ComPtr<Submission::ILogicalCommandList> logical;
    Microsoft::WRL::ComPtr<Submission::IRecordingResources> resources;
    if (FAILED(cmd->QueryInterface(IID_PPV_ARGS(&logical))) || logical->IsSplitIneligible() ||
        FAILED(cmd->QueryInterface(IID_PPV_ARGS(&resources))) || !resources->CanAppendCompute())
    { p->Status("mochizuki: recording boundary unavailable; original frame"); return nullptr; }
    if (!p->Ensure()) return nullptr;
    auto* cfg = Config::Instance();
    p->setLogging(cfg->LogToFile.value_or_default());
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
    frame.struct_size = sizeof frame;
    frame.frame_id = frameId;
    frame.flags = LMXXF_NR_FRAME_FLAG_STRENGTH | (temporal ? MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL : 0) |
        (first ? LMXXF_NR_FRAME_FLAG_FIRST_PASS : 0);
    const auto desc = input.colour->GetDesc();
    frame.color = input.colour;
    frame.color_width = input.width && input.width <= desc.Width ? input.width : UINT(desc.Width);
    frame.color_height = input.height && input.height <= desc.Height ? input.height : desc.Height;
    frame.color_state = input.colourState;
    // Jittered vectors cannot be used as the unjittered history flow without conversion.
    frame.motion = input.motionJittered ? nullptr : input.motion;
    frame.motion_state = input.motionState;
    frame.motion_width = input.motionWidth;
    frame.motion_height = input.motionHeight;
    frame.motion_scale_x = input.motionScaleX;
    frame.motion_scale_y = input.motionScaleY;
    frame.reset = input.reset;
    if (p->setControls(p->owner->context, &controls) != LMXXF_NR_OK) { p->Error("controls rejected"); return nullptr; }
    LmxxfNrJob job {sizeof job};
    if (p->prepare(p->owner->context, &frame, &job) != LMXXF_NR_OK)
    {
        p->owner->activity.Reset();
        char reason[256] {};
        p->api.GetLastError(reason, sizeof reason);
        p->Info();
        if (std::strstr(reason, "building the network"))
            p->Status("mochizuki: building the network; first use may take minutes (original frame)");
        else if (std::strstr(reason, "input resolution is settling"))
            p->Status("mochizuki: waiting for stable input resolution (original frame)");
        else
        {
            p->Status(std::string("mochizuki: ") + reason);
            if (++p->errors <= 3 || p->errors % 6000 == 0) LOG_WARN("mochizuki PrepareFrame: {}", reason);
        }
        return nullptr;
    }
    auto lease = LmxxfRecording::Attach(p->owner, job.handle, logical.Get());
    if (!lease) { p->Status("mochizuki: this recording already owns NR work"); return nullptr; }
    lease->neural = true;
    if (p->tracedRecordings < 2)
    {
        ++p->tracedRecordings;
        lease->traceId = frameId; lease->trace = SubmissionTrace;
        SubmissionTrace(frameId, "record.begin", cmd, 0);
    }
    auto* invocation = AmdPreSr::GraphicsSnap::GraphicsInvocationFor(reinterpret_cast<uint64_t>(cmd));
    if (invocation) { invocation->commandsRecorded = true; invocation->outcome = "recording_attempted"; }
    if (p->api.RecordInputs(p->owner->context, job.handle, cmd) != LMXXF_NR_OK ||
        LmxxfCut::TrySplitAtEvaluate(cmd) != S_OK ||
        p->api.RecordOutputs(p->owner->context, job.handle, cmd) != LMXXF_NR_OK)
    { p->failed = true; p->Error("recording failed"); return nullptr; }
    lease->ready = true;
    if (lease->trace) SubmissionTrace(frameId, "record.ready", cmd, 0);
    if (invocation) invocation->outcome = "recorded";
    p->Info();
    if (first) *first = static_cast<ID3D12Resource*>(job.first_pass_output);
    return static_cast<ID3D12Resource*>(job.private_output);
}
void MochizukiBackend::Submitted(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) { LmxxfRecording::Collect(); }
bool MochizukiBackend::PollRelease() { LmxxfRecording::Collect(); return true; }
void MochizukiBackend::ReleaseSession()
{
    std::lock_guard lifetime(Submission::RecordingMutex());
    p->owner.reset(); p->failed = false; p->retryAt = p->infoAt = 0;
    p->tracedRecordings = 0;
    LmxxfRecording::Collect();
    { std::lock_guard lock(p->mutex); p->timing = {}; p->progress = {}; p->details = {}; p->status = "mochizuki: NR off"; }
    if (!Config::Instance()->NrConvenience.value_or_default() && p->module)
    { FreeLibrary(p->module); p->module = nullptr; p->api = {}; }
}
bool MochizukiBackend::Shutdown()
{
    ReleaseSession();
    if (p->module) { FreeLibrary(p->module); p->module = nullptr; p->api = {}; }
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
}
