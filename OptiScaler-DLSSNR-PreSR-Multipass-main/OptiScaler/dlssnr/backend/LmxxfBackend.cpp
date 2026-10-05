#include "pch.h"
#include "../NrTimingDisplay.h"
#include "LmxxfBackend.h"
#include "InstallStatus.h"
#include "LmxxfRecordingOwner.h"
#include <cstring>
#include <State.h>
#include "../submission/SubmissionTls.h"
#include "lmxxf_runtime/LmxxfNrApi.h"
#include "../amd/AmdBridge.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>

namespace DlssNr::Backend
{
struct LmxxfBackend::Api
{
    LmxxfNrApi table {};
    LmxxfNrTimingApi timing {};
};

namespace
{
std::atomic<unsigned> g_lastColorH { 0 };
// Bounded phase evidence for the two reported RE titles, not a per-frame trace.
thread_local uint64_t reTraceId = 0;
void ReTrace(uint64_t id, const char* phase, const void* object, int32_t result) noexcept
{
    if (!id) return;
    try { LOG_DEBUG("RE NR phase: sample={} phase={} object={:p} result={}", id, phase, object, result); }
    catch (...) {}
}
struct ReTraceFrame
{
    uint64_t previous = reTraceId;
    ReTraceFrame(UINT width, UINT height, DXGI_FORMAT format)
    {
        reTraceId = 0;
        if (!spdlog::should_log(spdlog::level::debug)) return;
        const auto& exe = State::Instance().gameExe;
        if (_stricmp(exe.c_str(), "re9.exe") && _stricmp(exe.c_str(), "OnimushaWotS.exe")) return;
        // Record holds LifecycleMutex: these counters are serialized.
        static unsigned initial = 0, changes = 0;
        static UINT oldW = 0, oldH = 0;
        static DXGI_FORMAT oldFormat = DXGI_FORMAT_UNKNOWN;
        static uint64_t next = 0;
        const bool changed = width != oldW || height != oldH || format != oldFormat;
        oldW = width; oldH = height; oldFormat = format;
        if (initial < 3) { ++initial; reTraceId = ++next; }
        else if (changed && changes < 4) { ++changes; reTraceId = ++next; }
        ReTrace(reTraceId, "record.enter", nullptr, 0);
    }
    ~ReTraceFrame() { ReTrace(reTraceId, "record.return", nullptr, 0); reTraceId = previous; }
};

}

unsigned LastLmxxfColorHeight()
{
    return g_lastColorH.load(std::memory_order_relaxed);
}

namespace
{
std::wstring WidenPath(const std::filesystem::path &p) { return p.wstring(); }

std::filesystem::path ResolveModulesDir(const std::filesystem::path &directory)
{
    return FindLmxxfModuleRoot(directory);
}

// Each zero-output recovery blocks the game's submission thread on a GPU drain or clear.
// Past this many in a row, HIP is not coming back for this session.
constexpr uint32_t kMaxConsecutiveRecoveries = 10;

float CodecStrength(float v)
{
    return std::isfinite(v) ? std::clamp(v, 0.0f, 3.0f) : 1.0f;
}

// The NGX render subrect when it is usable, else the Colour allocation. The subrect is the
// normal case: UE at a non-native scale and dynamic resolution render into a corner of a larger
// buffer, and sizing the job by the allocation would feed the network the unrendered border and
// could push the input past the admission budget. The allocation is only the fallback for a
// missing (0) or impossible (larger than the texture) subrect, which is what the community
// Horizon patch was working around.
uint32_t JobExtent(uint32_t subrect, UINT64 texture)
{
    return (subrect > 0 && subrect <= texture) ? subrect : static_cast<uint32_t>(texture);
}

float CodecPaperWhite()
{
    const float v = Config::Instance()->LmxxfPaperWhite.value_or_default();
    return (std::isfinite(v) && v > 0.0f && v <= 64.0f) ? v : 1.0f;
}

// Games that pass a usable exposure texture use it. When there is none (Wo Long 2: AutoExposure +
// IsHdr), auto exposure asks the runtime to meter the colour and bind its own smoothed exposure
// (LMXXF_NR_FRAME_FLAG_AUTO_EXPOSURE); codec paper white stays the user's trim either way. Auto
// off uses the manual slider as a fixed divisor.
static bool IsUsableExposureTexture(void *res)
{
    if (!res)
        return false;
    const auto ed = static_cast<ID3D12Resource *>(res)->GetDesc();
    return ed.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && ed.Width == 1 && ed.Height == 1 &&
           ed.MipLevels == 1 && ed.DepthOrArraySize == 1 && ed.SampleDesc.Count == 1 &&
           (ed.Format == DXGI_FORMAT_R16_FLOAT || ed.Format == DXGI_FORMAT_R32_FLOAT) &&
           !(ed.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
}

// Pointer alone is not enough (Palworld NGX ExposureTexture is the wrong shape and the
// runtime drops it). Decide auto vs fixed from a usable texture. A host pre-exposure
// (DLSS_Pre_Exposure != 1) is treated as the game's exposure and skips the meter.
bool WantsAutoExposure(bool usableExposure, float preExposure = 1.0f)
{
    if (usableExposure)
        return false;
    if (std::isfinite(preExposure) && preExposure > 0.0f && std::fabs(preExposure - 1.0f) > 1e-3f)
        return false;
    return Config::Instance()->LmxxfAutoExposure.value_or_default();
}

float EffectiveCodecPaperWhite(bool usableExposure)
{
    // Pre-exposure skips the meter but does not force the manual divisor: paper white
    // follows Auto (codec trim) or Off (Exposure scale), same as with a game texture.
    if (usableExposure || Config::Instance()->LmxxfAutoExposure.value_or_default())
        return CodecPaperWhite();
    const float v = Config::Instance()->LmxxfAutoExposureScale.value_or_default();
    return (std::isfinite(v) && v > 0.0f && v <= 64.0f) ? v : 8.0f;
}

// The shader-side mean white point (auto-white.patch, debug_view bit 0x10000) is superseded by
// the runtime meter and no longer requested: the bit also broke the decode debug views, which
// compare the whole word against 1..4.
uint32_t CodecDebugViewBits(bool)
{
    return Config::Instance()->DlssNrDebugView.value_or_default() & 0xFu;
}

// Short, actionable menu copy for the common PrepareFrame fatals. Keep technical detail in OptiScaler.log.
const char *FriendlyPrepareFrameError(const char *err)
{
    if (!err || !err[0])
        return "lmxxf: session initialization failed";
    if (std::strstr(err, "NoBinaryForGpu") || std::strstr(err, "no binary for GPU") ||
        std::strstr(err, "hipErrorNoBinary") || std::strstr(err, "WrongDevice") ||
        std::strstr(err, "unsupported HIP architecture"))
    {
        return "lmxxf: this GPU is not supported by the installed lmxxf modules (need matching ISA, e.g. 9070 XT = gfx1201). Switch Backend to daniel, or install matching lmxxf-modules.";
    }
    if (std::strstr(err, "missing module architecture directory"))
    {
        return "lmxxf: modules missing subfolder for this GPU architecture. Check modules installation or reinstall lmxxf-modules.";
    }
    if (std::strstr(err, "stale flat"))
    {
        return "lmxxf: stale flat .hsaco files found in dual-architecture directory. Reinstall lmxxf-modules.";
    }
    if (std::strstr(err, "leaf SHA256SUMS mismatch"))
    {
        return "lmxxf: module leaf manifest mismatch with root. Reinstall lmxxf-modules.";
    }
    if (std::strstr(err, "dual-architecture directory missing") ||
        std::strstr(err, "incomplete") ||
        std::strstr(err, "fewer than 24"))
    {
        return "lmxxf: dual-architecture module set incomplete. Reinstall lmxxf-modules.";
    }
    if (std::strstr(err, "checksum mismatch") ||
        std::strstr(err, "failed to compute checksum") ||
        std::strstr(err, "invalid SHA256"))
    {
        return "lmxxf: module checksum verification failed. Reinstall lmxxf-modules or run sync.";
    }
    if (std::strstr(err, "unsafe module path"))
    {
        return "lmxxf: unsafe path detected in module manifest.";
    }
    if (std::strstr(err, "no HIP device matches"))
    {
        return "lmxxf: no HIP device matches D3D12 adapter. Switch Backend to daniel.";
    }
    if ((std::strstr(err, "missing") || std::strstr(err, "not found")) &&
        (std::strstr(err, "block") || std::strstr(err, ".f16") || std::strstr(err, ".f32") ||
         std::strstr(err, "weight")))
    {
        return "lmxxf: neural weights not found. Set LMXXF_WEIGHTS_DIR to native-game-tiled-assets (not HIP/), then restart the game.";
    }
    if (std::strstr(err, "weights") && (std::strstr(err, "not") || std::strstr(err, "unset")))
        return "lmxxf: neural weights not found. Set LMXXF_WEIGHTS_DIR to native-game-tiled-assets (not HIP/), then restart the game.";
    if (std::strstr(err, "poisoned"))
        return "lmxxf: NR is off after a fatal error (see OptiScaler.log). Fix the first error, then restart the game.";
    if (std::strstr(err, "hsaco") || std::strstr(err, "module"))
        return "lmxxf: lmxxf-modules failed to load on this GPU. Check modules vs GPU (gfx1200/1201) or use Backend daniel.";
    if (std::strstr(err, "Create:"))
        return "lmxxf: module initialization failed (see OptiScaler.log)";
    return "lmxxf: PrepareFrame failed";
}

bool IsPoisonedError(const char *err) { return err && std::strstr(err, "poisoned") != nullptr; }

} // namespace

void LmxxfBackend::SetStatus(const char *s)
{
    if (!s)
        return;
    if (status == s)
        return;
    status = s;
    // Surface to OptiScaler.log with progressive rate-limiting on status changes.
    static std::atomic<uint32_t> s_statusLogCount{0};
    const uint32_t c = s_statusLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (c <= 10 ||
        (c <= 100 && (c % 20 == 0)) ||
        (c <= 1000 && (c % 100 == 0)) ||
        (c % 1000 == 0))
    {
        LOG_INFO("lmxxf status: {} (status change #{})", status, c);
    }
}

LmxxfBackend::LmxxfBackend(ID3D12Device *dev, ID3D12CommandQueue *q, const std::filesystem::path &dir)
    : device(dev), queue(q), directory(dir)
{
    if (device)
        device->AddRef();
    if (queue)
        queue->AddRef();
    api = new Api();
    // Pending() is process-wide; count only recoveries that happen under this backend.
    seenRecoveries = LmxxfCut::Pending().recoveredEnqueues.load(std::memory_order_relaxed);
    seenEnqueueCalls = static_cast<uint64_t>(LmxxfCut::Pending().enqueueCalls.load(std::memory_order_relaxed));
    diagnostic = LmxxfProbe::ParseMode(Config::Instance()->LmxxfDiagnostic.value_or_default());
    LOG_INFO("lmxxf diagnostic: mode={} (restart to change; off/original/copy-current/staging-current/staging-previous/proxy-original/split-original/codec-passthrough/hip-passthrough)",
             Config::Instance()->LmxxfDiagnostic.value_or_default());
    {
        const bool fit = Config::Instance()->LmxxfFitLarge.value_or_default();
        CfgKey::PutEnvAlias(CfgKey::FitLarge, fit);
        LOG_INFO("lmxxf FitLarge={} ({}; NativeFitLargeInput re-reads env each call)", fit, CfgKey::EnvAlias(CfgKey::FitLarge));
    }
    {
        const bool allowEb = Config::Instance()->LmxxfAllowEnhancedBarriers.value_or_default();
        SetAllowEnhancedBarriers(allowEb);
        LOG_INFO("lmxxf enhanced barriers allowed={} (LmxxfAllowEnhancedBarriers)", allowEb);
    }
    {
        wchar_t srgb[8] {};
        const DWORD n = GetEnvironmentVariableW(L"DLSS5_CODEC_SRGB", srgb, 8);
        char val[16] {};
        if (n > 0 && n < 8)
            WideCharToMultiByte(CP_UTF8, 0, srgb, -1, val, sizeof(val), nullptr, nullptr);
        LOG_INFO("lmxxf codec sRGB env: {} (DLSS5_CODEC_SRGB={}; codec compile reads this once; 1=display-referred passthrough)",
                 val[0] ? "set" : "unset", val);
    }
    SetStatus("lmxxf: constructed (session not ready)");
}

LmxxfBackend::~LmxxfBackend()
{
    Shutdown();
    delete api;
    api = nullptr;
    if (queue)
    {
        queue->Release();
        queue = nullptr;
    }
    if (device)
    {
        device->Release();
        device = nullptr;
    }
}

bool LmxxfBackend::EnsureRuntime()
{
    if (runtimeDll && api && api->table.BeginRecordingExecution && api->table.CollectRecording)
        return true;
    const auto dllPath = directory / L"LmxxfNrRuntime.dll";
    if (runtimeDll) { FreeLibrary(reinterpret_cast<HMODULE>(runtimeDll)); runtimeDll = nullptr; }
    api->table = {};
    api->timing = {};
    runtimeDll = reinterpret_cast<void *>(LoadLibraryW(dllPath.c_str()));
    if (!runtimeDll)
    {
        SetStatus("lmxxf: LmxxfNrRuntime.dll missing next to OptiScaler");
        return false;
    }
    auto getApi = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrApi *)>(GetProcAddress(reinterpret_cast<HMODULE>(runtimeDll), "LmxxfNrGetApi"));
    if (!getApi)
    {
        SetStatus("lmxxf: LmxxfNrGetApi missing");
        return false;
    }
    api->table.struct_size = sizeof(LmxxfNrApi);
    if (getApi(LMXXF_NR_ABI_VERSION, &api->table) != LMXXF_NR_OK ||
        api->table.abi_version < 2 || !api->table.BeginRecordingExecution || !api->table.EndRecordingExecution ||
        !api->table.InvalidateRecording || !api->table.CollectRecording || !api->table.GetTimings)
    {
        api->table = {};
        SetStatus("lmxxf: runtime mismatch; replace the complete OptiScaler package");
        return false;
    }
    auto getTiming = reinterpret_cast<int32_t (*)(uint32_t, LmxxfNrTimingApi*)>(
        GetProcAddress(reinterpret_cast<HMODULE>(runtimeDll), "LmxxfNrGetTimingApi"));
    if (getTiming)
    {
        api->timing.struct_size = sizeof api->timing;
        if (getTiming(NR_TIMING_VERSION, &api->timing) != LMXXF_NR_OK ||
            api->timing.version != NR_TIMING_VERSION || !api->timing.SetEnabled || !api->timing.GetSnapshot)
            api->timing = {};
    }
    return true;
}

NrTimingSnapshot LmxxfBackend::Timing() const
{
    std::lock_guard lock(timingMutex);
    return timingSnapshot;
}

void LmxxfBackend::UpdateTiming()
{
    if (!session || !api->timing.GetSnapshot) return;
    const auto now = GetTickCount64();
    const bool enabled = Config::Instance()->NrTimingEnabled.value_or_default();
    const bool changed = !timingConfigured || enabled != timingEnabled;
    if (changed)
    {
        if (api->timing.SetEnabled(session, enabled ? 1u : 0u) != LMXXF_NR_OK) return;
        timingConfigured = true;
        timingEnabled = enabled;
        timingLogAt = 0;
    }
    if (enabled && api->table.GetTimings) {
        LmxxfNrTimings network {}; network.struct_size = sizeof network;
        api->table.GetTimings(session, &network); // render thread; never queried by the UI
    }
    if (!changed && (!enabled || now - timingReadAt < 500)) return;
    NrTimingSnapshot next {};
    next.struct_size = sizeof next;
    if (api->timing.GetSnapshot(session, &next) != LMXXF_NR_OK) return;
    timingReadAt = now;
    {
        std::lock_guard lock(timingMutex);
        timingSnapshot = next;
    }
    if (enabled && Config::Instance()->NrTimingLog.value_or_default() &&
        next.stages[NR_CPU_PREPARE].samples && (!timingLogAt || now - timingLogAt >= 5000))
    {
        timingLogAt = now;
        LOG_INFO("lmxxf timing: CPU prepare={} enqueue={} rebuild={} drain={} GPU encode={} NR+copy={} decode={} dropped={}",
            DlssNr::TimingValueText(next, NR_CPU_PREPARE, now, true),
            DlssNr::TimingValueText(next, NR_CPU_ENQUEUE, now, true),
            DlssNr::TimingValueText(next, NR_CPU_REBUILD, now, true),
            DlssNr::TimingValueText(next, NR_CPU_DRAIN, now, true),
            DlssNr::TimingValueText(next, NR_GPU_ENCODE, now, true),
            DlssNr::TimingValueText(next, NR_GPU_NETWORK, now, true),
            DlssNr::TimingValueText(next, NR_GPU_DECODE, now, true), next.dropped);
    }
}

bool LmxxfBackend::EnsureSession()
{
    if (sessionReady && session)
        return true;
    if (recoveryDisabled)
        return false;
    // A failed Create/PrepareSession retries with backoff, not (with its logs) on every frame.
    if (sessionRetryIn > 0)
    {
        --sessionRetryIn;
        return false;
    }
    if (!EnsureRuntime() || !device || !queue)
        return false;

    // Upstream 0.21+: auto picks 720/900/1080 from Color size. Unset defaults to 1080 and blacks 720p Color.
    if (!std::getenv("DLSS5_NETWORK_HEIGHT"))
    {
        _putenv("DLSS5_NETWORK_HEIGHT=auto");
        SetEnvironmentVariableA("DLSS5_NETWORK_HEIGHT", "auto");
        LOG_INFO("lmxxf: DLSS5_NETWORK_HEIGHT defaulted to auto");
    }
    const auto weights = FindLmxxfWeights(directory);
    if (weights.empty()) {
        SetStatus("lmxxf: weights missing. Install native-game-tiled-assets beside OptiScaler (not just HIP modules), then restart.");
        return false;
    }
    SetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", weights.c_str());
    LOG_INFO("lmxxf: weights directory={}", weights.string());

    const auto modules = ResolveModulesDir(directory);
    const std::wstring modulesW = WidenPath(modules);
    LOG_INFO("lmxxf: assets/modules dir={}", modules.string());

    static constexpr GUID kStreamlineRiid = { 0xADEC44E2, 0x61F0, 0x45C3, { 0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF } };
    IUnknown *qId = nullptr;
    if (queue)
        queue->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&qId));
    IUnknown *slQueue = nullptr;
    if (queue)
        queue->QueryInterface(kStreamlineRiid, reinterpret_cast<void **>(&slQueue));
    LOG_INFO("lmxxf: session queue={:p} (type {}) id={:p} sl={:p}",
             reinterpret_cast<void *>(queue),
             queue ? static_cast<int>(queue->GetDesc().Type) : -1,
             reinterpret_cast<void *>(qId),
             reinterpret_cast<void *>(slQueue));
    if (qId) qId->Release();
    if (slQueue) slQueue->Release();

    LmxxfNrCreateInfo info {};
    info.struct_size = sizeof(info);
    info.device = device;
    info.queue = queue;
    info.assets_directory = modulesW.c_str();
    info.flags = LMXXF_NR_CREATE_FLAG_RECORDING_LEASES;
    void *ctx = nullptr;
    int32_t createRc = api->table.Create(&info, &ctx);
    if (createRc != LMXXF_NR_OK || !ctx)
    {
        char err[256] {};
        if (api->table.GetLastError)
            api->table.GetLastError(err, sizeof err);
        LOG_ERROR("lmxxf: Create rc={} err={}", createRc, err);
        SetStatus(FriendlyPrepareFrameError(err));
        NoteSessionFailure();
        return false;
    }
    if (api->table.GetStatus)
    {
        char st[256] {};
        api->table.GetStatus(ctx, st, sizeof st);
        LOG_INFO("lmxxf: after Create status={}", st);
    }
    const int32_t prepRc = api->table.PrepareSession(ctx);
    if (prepRc != LMXXF_NR_OK)
    {
        char err[256] {};
        if (api->table.GetLastError)
            api->table.GetLastError(err, sizeof err);
        LOG_ERROR("lmxxf: PrepareSession rc={} err={}", prepRc, err);
        api->table.Destroy(ctx);
        SetStatus(FriendlyPrepareFrameError(err));
        NoteSessionFailure();
        return false;
    }
    sessionOwner = LmxxfRecording::SessionOwner::Create(api->table, ctx);
    if (!sessionOwner)
    {
        api->table.Destroy(ctx);
        SetStatus("lmxxf: cannot retain runtime for recording leases");
        return false;
    }
    session = ctx;
    timingConfigured = false;
    sessionReady = true;
    sessionFailures = 0;
    SetStatus("lmxxf: session ready");
    return true;
}

void LmxxfBackend::NoteSessionFailure()
{
    // 2, 4, 8 ... Record calls, capped near 10 s at 60 fps.
    ++sessionFailures;
    sessionRetryIn = std::min<uint32_t>(600u, 1u << std::min<uint32_t>(sessionFailures, 10u));
}

// Execution errors belong to the session owner, which can outlive this backend.
bool LmxxfBackend::NoteEnqueueRecoveries()
{
    if (recoveryDisabled) return false;
    if (!sessionOwner || !sessionOwner->failed) return true;
    recoveryDisabled = true;
    sessionOwner.reset(); session = nullptr; sessionReady = false;
    SetStatus("lmxxf: recording execution failed; NR off (see log)");
    return false;
}

ID3D12Resource *LmxxfBackend::FinishRecord(ID3D12GraphicsCommandList *recordCmd, void *jobHandle,
                                           void *privateOutput)
{
    DlssNr::Submission::ILogicalCommandList* logical = nullptr;
    if (FAILED(recordCmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                         reinterpret_cast<void**>(&logical))) || !logical)
    {
        api->table.InvalidateRecording(session, jobHandle);
        api->table.CollectRecording(session, jobHandle);
        SetStatus("lmxxf: recording proxy unavailable");
        return nullptr;
    }
    auto lease = LmxxfRecording::Attach(sessionOwner, jobHandle, logical);
    logical->Release();
    if (!lease) { SetStatus("lmxxf: this recording already owns an NR job"); return nullptr; }
    lease->traceId = reTraceId;
    lease->trace = &ReTrace;
    ReTrace(reTraceId, "inputs.begin", jobHandle, 0);
    if (api->table.RecordInputs(session, jobHandle, recordCmd) != LMXXF_NR_OK)
    {
        sessionOwner->failed = true;
        SetStatus("lmxxf: RecordInputs failed");
        return nullptr;
    }
    ReTrace(reTraceId, "inputs.end/split.begin", jobHandle, 0);
    const HRESULT splitHr = LmxxfCut::TrySplitAtEvaluate(recordCmd);
    ReTrace(reTraceId, "split.end", recordCmd, splitHr);
    if (splitHr != S_OK)
    {
        sessionOwner->failed = true;
        SetStatus("lmxxf: Split failed");
        return nullptr;
    }
    ReTrace(reTraceId, "outputs.begin", jobHandle, 0);
    if (api->table.RecordOutputs(session, jobHandle, recordCmd) != LMXXF_NR_OK)
    {
        sessionOwner->failed = true;
        SetStatus("lmxxf: RecordOutputs failed");
        return nullptr;
    }
    ReTrace(reTraceId, "outputs.end", jobHandle, 0);
    lease->ready = true;
    SetStatus(diagnostic == LmxxfProbe::Mode::HipPassthrough ?
                  "lmxxf diagnostic: hip-passthrough (HIP round trip; NO NR)" :
              diagnostic == LmxxfProbe::Mode::CodecPassthrough ?
                  "lmxxf diagnostic: codec-passthrough (NO HIP/NR)" : "lmxxf: recording ready");
    return reinterpret_cast<ID3D12Resource *>(privateOutput);
}

ID3D12Resource *LmxxfBackend::Record(ID3D12GraphicsCommandList *cmd, const AmdPreSr::Frame &frame,
                                     const AmdPreSr::Settings &settings)
{
    std::lock_guard recordLock(LmxxfCut::LifecycleMutex());
    if (!PollRelease()) return nullptr;
    if (!cmd || !frame.colour)
    {
        SetStatus("lmxxf: Record missing cmd/colour");
        return nullptr;
    }
    // Crucially before EnsureSession: controls do not load the runtime, prepare HIP,
    // or submit HIP. split-original only cuts the game list for boundary validation.
    if (diagnostic != LmxxfProbe::Mode::Off)
        return RecordDiagnostic(cmd, frame, settings);
    // Never substitute Color from an earlier Evaluate to work around an unsubmitted producer.
    DlssNr::Submission::ILogicalCommandList *logical = nullptr;
    if (FAILED(cmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                  reinterpret_cast<void **>(&logical))) || !logical)
    {
        SetStatus("lmxxf: same-frame boundary unavailable (original Color; NO NR)");
        return nullptr;
    }
    const bool ineligible = logical->IsSplitIneligible();
    const char *reason = logical->SplitRejectionReason();
    logical->Release();
    if (ineligible)
    {
        char status[128] {};
        std::snprintf(status, sizeof(status), "lmxxf: split ineligible: %s (NO NR)",
                      reason ? reason : "unknown");
        SetStatus(status);
        static std::atomic<uint32_t> s_ineligibleCount{0};
        const uint32_t c = s_ineligibleCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (c <= 10 ||
            (c <= 100 && (c % 20 == 0)) ||
            (c <= 1000 && (c % 100 == 0)) ||
            (c % 1000 == 0))
        {
            LOG_WARN("{} (frame #{})", status, c);
        }
        return nullptr;
    }
    if (!NoteEnqueueRecoveries())
        return nullptr;
    const auto traceDesc = frame.colour->GetDesc();
    ReTraceFrame traceFrame(frame.width, frame.height, traceDesc.Format);
    ReTrace(reTraceId, "session.begin", session, 0);
    if (!EnsureSession())
        return nullptr;
    ReTrace(reTraceId, "session.end", session, 0);
    UpdateTiming();

    D3D12_RESOURCE_DESC desc = frame.colour->GetDesc();
    LmxxfNrFrameInfo fi {};
    fi.struct_size = sizeof(fi);
    fi.frame_id = ++frameId;
    fi.command_list = cmd;
    fi.color_width = JobExtent(frame.width, desc.Width);
    fi.color_height = JobExtent(frame.height, desc.Height);
    g_lastColorH.store(fi.color_height, std::memory_order_relaxed);
    fi.color = frame.colour;
    fi.color_state = static_cast<uint32_t>(frame.colourState);
    fi.flags = LMXXF_NR_FRAME_FLAG_STRENGTH | LMXXF_NR_FRAME_FLAG_DEBUG_VIEW;
    fi.transfer_strength = CodecStrength(Config::Instance()->DlssNrTransferStrength.value_or_default());
    fi.color_strength = CodecStrength(Config::Instance()->DlssNrColourStrength.value_or_default());
    fi.model_scale = settings.modelScale;
    fi.paper_white = EffectiveCodecPaperWhite(IsUsableExposureTexture(frame.exposure));
    fi.debug_view = CodecDebugViewBits(IsUsableExposureTexture(frame.exposure));
    if (WantsAutoExposure(IsUsableExposureTexture(frame.exposure), frame.preExposure))
        fi.flags |= LMXXF_NR_FRAME_FLAG_AUTO_EXPOSURE;
    // AmdBridge already collects these from the NGX parameters (ExposureTexture,
    // DLSS_Pre_Exposure, DLSS_Exposure_Scale); they only needed to cross the C ABI.
    fi.exposure = frame.exposure;
    fi.exposure_state = static_cast<uint32_t>(frame.exposureState);
    fi.pre_exposure = frame.preExposure;
    fi.exposure_scale = frame.exposureScale;

    LmxxfNrJob job {};
    job.struct_size = sizeof(job);
    ReTrace(reTraceId, "prepare.begin", session, 0);
    int32_t frameRc = api->table.PrepareFrame(session, &fi, &job);
    ReTrace(reTraceId, "prepare.end", job.handle, frameRc);
    if (frameRc != LMXXF_NR_OK || !job.handle || !job.private_output)
    {
        char err[256] {};
        if (api->table.GetLastError)
            api->table.GetLastError(err, sizeof err);
        const auto enqueue = LmxxfCut::LastEnqueueDiagnostic();
        static unsigned prepareFrameRebuilds = 0;
        static constexpr GUID kStreamlineRiid = { 0xADEC44E2, 0x61F0, 0x45C3, { 0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF } };
        IUnknown *sessId = nullptr;
        if (queue)
            queue->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&sessId));
        IUnknown *sessSl = nullptr;
        if (queue)
            queue->QueryInterface(kStreamlineRiid, reinterpret_cast<void **>(&sessSl));

        IUnknown *execId = nullptr;
        if (enqueue.queue)
            enqueue.queue->QueryInterface(IID_IUnknown, reinterpret_cast<void **>(&execId));
        IUnknown *execSl = nullptr;
        if (enqueue.queue)
            enqueue.queue->QueryInterface(kStreamlineRiid, reinterpret_cast<void **>(&execSl));

        const bool poisoned = IsPoisonedError(err);
        // First failure is the diagnosis; a poisoned session cannot recover this launch.
        // Keep later repeats quiet so OptiScaler.log stays readable (summary is printed on Shutdown).
        const unsigned failN = ++prepareFrameFailLogs;
        if (poisoned)
            ++prepareFramePoisonLogs;
        const bool shouldLog = (failN == 1) || (!poisoned && (failN <= 3 || (failN % 60) == 0)) ||
                               (poisoned && (failN % 2000) == 0);
        if (shouldLog)
            LOG_ERROR("lmxxf: PrepareFrame rc={} handle={} out={} err={} lastEnqueueRc={:X} lastEnqueueErr={} sessQ={:p}(t={},id={:p},sl={:p}) execQ={:p}(t={},id={:p},sl={:p}) {}x{} (fail#{})",
                      frameRc, job.handle != nullptr, job.private_output != nullptr, err,
                      enqueue.rc, enqueue.error.data(),
                      reinterpret_cast<void *>(queue), queue ? static_cast<int>(queue->GetDesc().Type) : -1,
                      reinterpret_cast<void *>(sessId), reinterpret_cast<void *>(sessSl),
                      reinterpret_cast<void *>(enqueue.queue), enqueue.queue ? static_cast<int>(enqueue.queue->GetDesc().Type) : -1,
                      reinterpret_cast<void *>(execId), reinterpret_cast<void *>(execSl),
                      fi.color_width, fi.color_height,
                      failN);

        if (sessId) sessId->Release();
        if (sessSl) sessSl->Release();
        if (execId) execId->Release();
        if (execSl) execSl->Release();
        // Menu/resize: runtime drains/rebuilds codec on rebind/geometry; if still failing,
        // drop host session so the next Record EnsureSession starts clean. An input contract
        // violation is INVALID_ARGUMENT and a rebuild cannot help it, so never rebuild for it.
        const bool rebindish = frameRc != LMXXF_NR_INVALID_ARGUMENT &&
                               (err[0] && (std::strstr(err, "rebind") || std::strstr(err, "geometry")));
        if (rebindish && (prepareFrameFailLogs <= 2 || (prepareFrameFailLogs % 4) == 0))
        {
            LOG_WARN("lmxxf: PrepareFrame fail -> host session rebuild #{}", ++prepareFrameRebuilds);
            sessionOwner.reset();
            session = nullptr;
            sessionReady = false;
            SetStatus("lmxxf: session rebuild after PrepareFrame fail");
        }
        else
        {
            SetStatus(FriendlyPrepareFrameError(err));
        }
        return nullptr;
    }
    {
        // Surface runtime codec-recreate diagnostics into OptiScaler.log (rate-limited).
        // Must read GetLastError BEFORE GetStatus - GetStatus clears the last-error slot.
        // A successful PrepareFrame may still leave a notice (codec recreate, unusable exposure).
        char noticeMsg[320] {};
        if (api->table.GetLastError)
            api->table.GetLastError(noticeMsg, sizeof noticeMsg);
        if (noticeMsg[0])
        {
            static unsigned noticeLogs = 0;
            if (noticeLogs < 8 || (noticeLogs % 30) == 0)
                LOG_INFO("{}", noticeMsg);
            ++noticeLogs;
        }
        static bool loggedGeo = false;
        if (!loggedGeo && api->table.GetStatus)
        {
            char st[256] {};
            api->table.GetStatus(session, st, sizeof st);
            LOG_INFO("lmxxf: after PrepareFrame HIP/net geometry status={}", st);
            loggedGeo = true;
        }
        // Highlight triage: exposure and colour contract, not every frame.
        {
            static unsigned colorDiagN = 0;
            ++colorDiagN;
            if (spdlog::should_log(spdlog::level::debug) && (colorDiagN <= 8 || (colorDiagN % 300) == 0))
            {
                wchar_t srgbEnv[8] {};
                const DWORD n = GetEnvironmentVariableW(L"DLSS5_CODEC_SRGB", srgbEnv, 8);
                char srgbVal[8] {};
                if (n > 0 && n < 8)
                    WideCharToMultiByte(CP_UTF8, 0, srgbEnv, -1, srgbVal, sizeof(srgbVal), nullptr, nullptr);
                LOG_DEBUG("lmxxf color: fmt={} {}x{} alloc={}x{} exposure={} expState={} preExposure={:.6g} "
                         "exposureScale={:.6g} paperWhite={:.6g} transfer={:.3f} colour={:.3f} srgbEnv={}",
                         static_cast<unsigned>(desc.Format), fi.color_width, fi.color_height,
                         static_cast<unsigned>(desc.Width), static_cast<unsigned>(desc.Height),
                         static_cast<void *>(fi.exposure), fi.exposure_state, fi.pre_exposure,
                         fi.exposure_scale, fi.paper_white, fi.transfer_strength, fi.color_strength,
                         srgbVal[0] ? srgbVal : "unset");
            }
        }
    }

    ID3D12Resource *result = FinishRecord(cmd, job.handle, job.private_output);
    static uint64_t recordEvalCount = 0;
    const auto rEval = ++recordEvalCount;
    auto &p = LmxxfCut::Pending();
    const auto lastRc = static_cast<unsigned>(p.lastEnqueueRc.load(std::memory_order_relaxed));
    const auto submitFails = DlssNr::Submission::g_submissionFailures.load(std::memory_order_relaxed);
    const bool newSubmitFailure = submitFails != loggedSubmissionFailures;
    loggedSubmissionFailures = submitFails;
    const bool enqueueError = lastRc != 0 && lastRc != static_cast<unsigned>(LmxxfCut::kEnqueueSkipped) &&
                              lastRc != static_cast<unsigned>(LmxxfCut::kEnqueueQueueMismatch);

    if (enqueueError || newSubmitFailure)
    {
        lastAnomalyTime = GetTickCount64();
        const uint64_t key = enqueueError ? uint64_t(lastRc) : (uint64_t(1) << 32);
        if (anomalyLog.Allow(lastAnomalyTime, key))
            LOG_WARN("lmxxf nr anomaly: eval={} lastEnqueueRc={:X} submitFailures={} skippedHits={} suppressed={}",
                     rEval, lastRc, submitFails, p.skippedHits.load(std::memory_order_relaxed),
                     anomalyLog.TakeSuppressed());
    }
    else if (anomalyLog.Active() && GetTickCount64() - lastAnomalyTime >= 5000)
    {
        LOG_INFO("lmxxf nr: no new submission errors; eval={} suppressed={}", rEval, anomalyLog.TakeSuppressed());
        anomalyLog.Reset();
    }

    if (rEval <= 5)
    {
        LOG_INFO("lmxxf nr: eval={} output={} betweenHits={} enqueueCalls={} skippedHits={} lastEnqueueRc={:X} transfer={:.2f} color={:.2f} debugView={} scale={:.2f} producerSubmitted={} continuationSubmitted={} submitFailures={}",
                 rEval, static_cast<void *>(result),
                 p.betweenHits.load(std::memory_order_relaxed),
                 p.enqueueCalls.load(std::memory_order_relaxed),
                 p.skippedHits.load(std::memory_order_relaxed),
                 lastRc,
                 fi.transfer_strength, fi.color_strength, fi.debug_view, fi.model_scale,
                 DlssNr::Submission::g_splitSubmissions.load(std::memory_order_relaxed),
                 DlssNr::Submission::g_continuationSubmissions.load(std::memory_order_relaxed),
                 submitFails);
    }
    else if (rEval % 120 == 0)
    {
        LOG_DEBUG("lmxxf nr: eval={} output={} betweenHits={} enqueueCalls={} skippedHits={} lastEnqueueRc={:X} transfer={:.2f} color={:.2f} debugView={} scale={:.2f} producerSubmitted={} continuationSubmitted={} submitFailures={}",
                  rEval, static_cast<void *>(result),
                  p.betweenHits.load(std::memory_order_relaxed),
                  p.enqueueCalls.load(std::memory_order_relaxed),
                  p.skippedHits.load(std::memory_order_relaxed),
                  lastRc,
                  fi.transfer_strength, fi.color_strength, fi.debug_view, fi.model_scale,
                  DlssNr::Submission::g_splitSubmissions.load(std::memory_order_relaxed),
                  DlssNr::Submission::g_continuationSubmissions.load(std::memory_order_relaxed),
                  submitFails);
    }
    return result;
}


ID3D12Resource *LmxxfBackend::RecordDiagnostic(ID3D12GraphicsCommandList *cmd, const AmdPreSr::Frame &frame,
                                               const AmdPreSr::Settings &settings)
{
    const auto seq = ++evaluateSequence_;
    const auto id = ++probeEvaluateId;
    const bool sampled = id <= 3 || id % 120 == 0;
    const auto d = frame.colour->GetDesc();
    const UINT w = frame.width ? frame.width : static_cast<UINT>(d.Width);
    const UINT h = frame.height ? frame.height : d.Height;
    ID3D12Resource *output = nullptr;
    const char *reason = "original_no_nr";
    LmxxfProbe::Evidence ev {};
    ev.evaluateId = id;
    ev.list = cmd;
    ev.sampled = sampled;
    ev.mode = diagnostic;

    if (diagnostic == LmxxfProbe::Mode::CodecPassthrough || diagnostic == LmxxfProbe::Mode::HipPassthrough)
    {
        const bool hipPassthrough = diagnostic == LmxxfProbe::Mode::HipPassthrough;
        const char* modeName = hipPassthrough ? "hip-passthrough" : "codec-passthrough";
        DlssNr::Submission::ILogicalCommandList *logical = nullptr;
        const bool isProxy = SUCCEEDED(cmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                                         reinterpret_cast<void **>(&logical))) && logical;
        if (!isProxy)
        {
            reason = "boundary_not_proxy";
            ++boundaryRejects;
            SetStatus((std::string("lmxxf diagnostic: ") + modeName + " REJECTED (not proxy; original Color)").c_str());
        }
        else
        {
            ++boundaryProxyHits;
            if (logical->IsSplitIneligible())
            {
                reason = logical->SplitRejectionReason();
                ++boundaryRejects;
                char state[256] {};
                snprintf(state, sizeof state, "lmxxf diagnostic: %s REJECTED (%s; original Color)", modeName, reason);
                SetStatus(state);
            }
            else if (!EnsureSession())
            {
                reason = "ensure_session_failed";
                ++boundaryRejects;
                SetStatus((std::string("lmxxf diagnostic: ") + modeName + " REJECTED (session failed; original Color)").c_str());
            }
            else
            {
                D3D12_RESOURCE_DESC desc = frame.colour->GetDesc();
                LmxxfNrFrameInfo fi {};
                fi.struct_size = sizeof(fi);
                fi.frame_id = ++frameId;
                fi.command_list = cmd;
                fi.color_width = JobExtent(frame.width, desc.Width);
                fi.color_height = JobExtent(frame.height, desc.Height);
                fi.color = frame.colour;
                fi.color_state = static_cast<uint32_t>(frame.colourState);
                fi.flags = LMXXF_NR_FRAME_FLAG_STRENGTH | LMXXF_NR_FRAME_FLAG_DEBUG_VIEW |
                           (hipPassthrough ? LMXXF_NR_FRAME_FLAG_HIP_PASSTHROUGH : LMXXF_NR_FRAME_FLAG_CODEC_PASSTHROUGH);
                fi.transfer_strength = CodecStrength(Config::Instance()->DlssNrTransferStrength.value_or_default());
                fi.color_strength = CodecStrength(Config::Instance()->DlssNrColourStrength.value_or_default());
                fi.paper_white = EffectiveCodecPaperWhite(IsUsableExposureTexture(frame.exposure));
                fi.debug_view = CodecDebugViewBits(IsUsableExposureTexture(frame.exposure));
                if (WantsAutoExposure(IsUsableExposureTexture(frame.exposure), frame.preExposure))
                    fi.flags |= LMXXF_NR_FRAME_FLAG_AUTO_EXPOSURE;
                fi.model_scale = settings.modelScale;
                // Same frame contract as the normal path: without these, passthrough
                // is not a clean A/B of "network off" for highlight/exposure bugs.
                fi.exposure = frame.exposure;
                fi.exposure_state = static_cast<uint32_t>(frame.exposureState);
                fi.pre_exposure = frame.preExposure;
                fi.exposure_scale = frame.exposureScale;

                LmxxfNrJob job {};
                job.struct_size = sizeof(job);
                const int32_t frameRc = api->table.PrepareFrame(session, &fi, &job);
                if (frameRc != LMXXF_NR_OK || !job.handle || !job.private_output)
                {
                    char err[256] {};
                    if (api->table.GetLastError)
                        api->table.GetLastError(err, sizeof err);
                    static uint64_t diagPrepareFails = 0;
                    if (++diagPrepareFails <= 5 || (diagPrepareFails % 300 == 0))
                    {
                        LOG_ERROR("lmxxf diagnostic: {} PrepareFrame rc={} handle={} out={} err='{}' {}x{} (fail#{})",
                                  modeName, frameRc, job.handle != nullptr, job.private_output != nullptr, err,
                                  fi.color_width, fi.color_height, diagPrepareFails);
                    }
                    reason = "prepare_frame_failed";
                    ++boundaryRejects;
                    SetStatus((std::string("lmxxf diagnostic: ") + modeName + " PrepareFrame failed: " + err +
                               (hipPassthrough && frameRc == LMXXF_NR_INVALID_ARGUMENT &&
                                std::strstr(err, "unknown flags") ?
                                "; update the complete package (host and runtime must match)" : "")).c_str());
                }
                else
                {
                    output = FinishRecord(cmd, job.handle, job.private_output);
                    reason = hipPassthrough ? (output ? "hip_passthrough_recorded" : "hip_passthrough_record_failed") :
                                             (output ? "codec_passthrough_recorded" : "codec_passthrough_record_failed");
                    if (output) ++boundaryCuts; else ++boundaryRejects;
                }
            }
        }
        if (logical)
            logical->Release();
        if (sampled)
        {
            LOG_INFO("lmxxf boundary: eval={} proxy={} reason={} proxyHits={} cutsRecorded={} rejected={} output={} unsplitSubmitted={} producerSubmitted={} continuationSubmitted={} submitFailures={}",
                     id, isProxy, reason, boundaryProxyHits, boundaryCuts, boundaryRejects, static_cast<void *>(output),
                     DlssNr::Submission::g_unsplitProxySubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_splitSubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_continuationSubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_submissionFailures.load(std::memory_order_relaxed));
            if (hipPassthrough && session && api->table.GetStatus)
            {
                char status[1536] {};
                api->table.GetStatus(session, status, sizeof status);
                LOG_INFO("lmxxf HIP passthrough: eval={} enqueueCalls={} lastEnqueueRc={} runtime='{}' (queued counts, not GPU completion)",
                         id, LmxxfCut::Pending().enqueueCalls.load(), LmxxfCut::Pending().lastEnqueueRc.load(), status);
            }
        }
    }
    else if (LmxxfProbe::NeedsOpenListProxy(diagnostic))
    {
        DlssNr::Submission::ILogicalCommandList *logical = nullptr;
        HRESULT cutHr = S_FALSE;
        const bool isProxy = SUCCEEDED(cmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                                         reinterpret_cast<void **>(&logical))) && logical;
        if (!isProxy)
            reason = "boundary_not_proxy";
        else
        {
            ++boundaryProxyHits;
            if (diagnostic == LmxxfProbe::Mode::ProxyOriginal)
                reason = "proxy_original_no_nr";
            else if (logical->IsSplitIneligible())
                reason = logical->SplitRejectionReason();
            else
            {
                cutHr = logical->SplitSegments();
                if (cutHr == S_OK)
                {
                    ++boundaryCuts;
                    reason = "split_original_recorded_no_nr";
                }
                else
                    reason = "boundary_split_failed";
            }
        }
        if (logical)
            logical->Release();
        if (!isProxy || (diagnostic == LmxxfProbe::Mode::SplitOriginal && cutHr != S_OK))
            ++boundaryRejects;
        char state[256] {};
        snprintf(state, sizeof state, "lmxxf diagnostic: %s (original Color; NO NR)", reason);
        SetStatus(state);
        if (sampled)
            LOG_INFO("lmxxf boundary: eval={} proxy={} cutHr={:X} proxyHits={} cutsRecorded={} rejected={} unsplitSubmitted={} producerSubmitted={} continuationSubmitted={} submitFailures={} (counts are NOT GPU completion)",
                     id, isProxy, static_cast<unsigned>(cutHr), boundaryProxyHits, boundaryCuts, boundaryRejects,
                     DlssNr::Submission::g_unsplitProxySubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_splitSubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_continuationSubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_submissionFailures.load(std::memory_order_relaxed));
    }
    else if (diagnostic == LmxxfProbe::Mode::CopyCurrent)
    {
        output = colorProbe.Record(device, cmd, frame.colour, frame.colourState, w, h);
        reason = colorProbe.Reason();
        SetStatus(output ? "lmxxf diagnostic: copy-current (NO NR; recorded, not GPU-complete)"
                         : "lmxxf diagnostic: copy-current REJECTED (original Color; see log)");
    }
    else if (diagnostic == LmxxfProbe::Mode::StagingCurrent || diagnostic == LmxxfProbe::Mode::StagingPrevious)
    {
        auto r = stagingProbe.Record(diagnostic, device, cmd, frame.colour, frame.colourState, w, h, seq);
        output = r.output;
        reason = r.reason;
        ev.epoch = r.epoch;
        ev.sourceSequence = r.sourceSequence;
        ev.age = r.age;
        ev.priming = r.priming;
        const char *modeName = (diagnostic == LmxxfProbe::Mode::StagingCurrent) ? "staging-current" : "staging-previous";
        if (r.priming)
        {
            char buf[256];
            snprintf(buf, sizeof buf, "lmxxf diagnostic: %s PRIMING (no previous; original Color)", modeName);
            SetStatus(buf);
        }
        else if (output)
        {
            char buf[256];
            snprintf(buf, sizeof buf, "lmxxf diagnostic: %s (NO NR; age=%u src=%llu)",
                     modeName, r.age, static_cast<unsigned long long>(r.sourceSequence));
            SetStatus(buf);
        }
        else
        {
            char buf[256];
            snprintf(buf, sizeof buf, "lmxxf diagnostic: %s REJECTED (%s)", modeName, reason);
            SetStatus(buf);
        }
    }
    else if (diagnostic == LmxxfProbe::Mode::Original)
        SetStatus("lmxxf diagnostic: original (NO NR, no replacement)");
    else
    {
        reason = "invalid_diagnostic_option";
        SetStatus("lmxxf diagnostic: INVALID option (original Color; no HIP)");
    }

    ev.expectedColor = output ? output : frame.colour;
    ev.copied = output != nullptr;
    LmxxfProbe::CurrentEvidence() = ev;

    if (sampled)
    {
        const auto st = stagingProbe.GetStats();
        LOG_INFO("lmxxf probe: eval={} seq={} list={} color={} output={} mv={} depth={} valid={}x{} allocation={}x{} format={} colorState={} preExposure={} exposureScale={} reset={} reason={} mode={} epoch={} age={} srcSeq={} priming={} window={} best={} pins={} bytes={} (CPU record only)",
                 id, seq, static_cast<void *>(cmd), static_cast<void *>(frame.colour), static_cast<void *>(output),
                 static_cast<void *>(frame.motion), static_cast<void *>(frame.depth), w, h, d.Width, d.Height,
                 static_cast<unsigned>(d.Format), static_cast<unsigned>(frame.colourState), frame.preExposure,
                 frame.exposureScale, frame.reset, reason, static_cast<int>(diagnostic),
                 ev.epoch, ev.age, ev.sourceSequence, ev.priming,
                 st.currentWindowLength, st.bestWindowLength,
                 stagingProbe.CaptureCount() + stagingProbe.OutputCount() + colorProbe.EntryCount(),
                 stagingProbe.AllocatedBytes() + colorProbe.AllocatedBytes());
    }
    return output;
}

// Daniel uses PendingListIndex to isolate a private neural list from a multi-list batch.
// lmxxf HIP sits in ExecuteExpanded's between-slot on the game proxy itself, so isolation
// is unnecessary: AmdBridge::ExecuteBatch always calls ExecuteExpanded when ExpandEnabled().
int LmxxfBackend::PendingListIndex(UINT, ID3D12CommandList *const *) const { return -1; }

void LmxxfBackend::Submitting(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *) {}

void LmxxfBackend::TraceBoundary(const std::string &) {}

void LmxxfBackend::Submitted(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *)
{
    // Original batches do not establish execution or invalidate closed recordings.
    // Recording observers report the actual producer/consumer and tail Signal.
    LmxxfRecording::Collect();
}

bool LmxxfBackend::Shutdown()
{
    std::lock_guard lifetime(LmxxfCut::LifecycleMutex());
    LmxxfCut::DisarmBetweenSlot();
    sessionOwner.reset(); session = nullptr; sessionReady = false;
    LmxxfRecording::Collect();
    if (runtimeDll) { FreeLibrary(reinterpret_cast<HMODULE>(runtimeDll)); runtimeDll = nullptr; }
    if (api) api->table = {};
    SetStatus("lmxxf: shutdown");
    return true;
}

void LmxxfBackend::ReleaseSession()
{
    std::lock_guard lifetime(LmxxfCut::LifecycleMutex());
    {
        std::lock_guard lock(timingMutex);
        timingSnapshot = {};
    }
    timingConfigured = false;
    // Existing recording observers retain their own session/module. New active
    // sessions can start immediately; old closed lists are still executable.
    sessionOwner.reset(); session = nullptr; sessionReady = false;
    sessionFailures = sessionRetryIn = 0; recoveryDisabled = false;
    LmxxfRecording::Collect();
    if (!Config::Instance()->NrConvenience.value_or_default())
    {
        if (runtimeDll) FreeLibrary(reinterpret_cast<HMODULE>(runtimeDll));
        runtimeDll = nullptr;
        if (api) api->table = {};
    }
    SetStatus("lmxxf: NR off (live recordings retain their resources)");
}

bool LmxxfBackend::PollRelease()
{
    LmxxfRecording::Collect();
    return true;
}

void LmxxfBackend::ResetGraphicsWaitState()
{
    // lmxxf does not use daniel's graphics PSO wait. Keep the no-op explicit so
    // Host callers can treat both backends the same on a switch.
}

void LmxxfBackend::InvalidateHistory()
{
    std::lock_guard lifetime(LmxxfCut::LifecycleMutex());
    if (session && api && api->table.ResetHistory)
    {
        const int32_t resetRc = api->table.ResetHistory(session);
        if (resetRc != LMXXF_NR_OK)
        {
            static std::atomic<uint32_t> resetFailures { 0 };
            const uint32_t n = resetFailures.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 3 || n % 120 == 0)
            {
                char err[256] {};
                if (api->table.GetLastError)
                    api->table.GetLastError(err, sizeof err);
                LOG_ERROR("lmxxf: ResetHistory rc={} err={} (fail#{})", resetRc, err, n);
            }
        }
    }
    stagingProbe.InvalidateEpoch();
}

std::string LmxxfBackend::Status() const { return status; }

bool LmxxfBackend::GraphicsRestartNeeded(UINT) const { return false; }
} // namespace DlssNr::Backend
