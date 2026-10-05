#include "pch.h"
#include "AmdBridge.h"
#include "../DiagnosticLog.h"
#include "AwaitingListTracker.h"
#include "../submission/SubmissionTls.h"
#include "AmdPreSr.h"
#include "PresentExperimental.h"
#include "../backend/DanielBackend.h"
#include "../effects/NrOutputEffects.h"
#include "../PostSr.h"
#include "../backend/LmxxfBackend.h"
#include "../backend/MochizukiBackend.h"
#include "../backend/Selector.h"
#include "../backend/LmxxfEvaluateCut.h"
#include "../backend/LmxxfGenerationObserver.h"
#include "../submission/SubmissionHooks.h"
#include <State.h>
#include <Util.h>
#include <misc/SkipSpoof.h>
#include <detours/detours.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>

namespace DlssNr::AmdBridge
{
namespace
{
// Both hosts stay alive once built (Daniel HIP threads are process-lifetime).
// Switching only changes which one Record/Submit uses.
std::atomic<DlssNr::Backend::Host*> g_daniel { nullptr };
std::atomic<DlssNr::Backend::Host*> g_lmxxf { nullptr };
std::atomic<DlssNr::Backend::MochizukiBackend*> g_mochizuki { nullptr };

DlssNr::Backend::Host* HostForKind(DlssNr::Backend::Kind k)
{
    if (k == DlssNr::Backend::Kind::Mochizuki) return g_mochizuki.load(std::memory_order_acquire);
    if (k == DlssNr::Backend::Kind::Lmxxf)
        return g_lmxxf.load(std::memory_order_acquire);
    return g_daniel.load(std::memory_order_acquire);
}
// Execute is on the submit path: do not re-resolve NrBackend/disk every batch.
// -1 = unknown; SyncBackendWithConfig and first use fill it in.
std::atomic<int> g_activeKind { -1 };
DlssNr::Backend::Kind ActiveKindCached()
{
    int k = g_activeKind.load(std::memory_order_acquire);
    if (k < 0)
    {
        k = static_cast<int>(DlssNr::Backend::ActiveKindFromConfig());
        int unknown = -1;
        if (!g_activeKind.compare_exchange_strong(unknown, k, std::memory_order_acq_rel))
            k = unknown;
    }
    return static_cast<DlssNr::Backend::Kind>(k);
}
DlssNr::Backend::Host* ActiveHost()
{
    return HostForKind(ActiveKindCached());
}
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using ExitFn = void(NTAPI*)(LONG);
ExecuteFn executeOriginal = nullptr;
ExitFn exitOriginal = nullptr;
bool submissionHookReady = false; // guarded by initMutex
std::string message = "AMD NR: waiting for a DirectX 12 SR frame";
std::mutex messageMutex;
std::mutex initMutex;
std::mutex observedMutex;
std::unordered_set<ID3D12CommandList*> observedLists;
void Message(const char* s)
{
    std::lock_guard l(messageMutex);
    // Evaluate clears the visible message before retrying. A persistent buffer
    // or hook limitation must not append the same failure at frame rate.
    static std::string lastLogged;
    static ULONGLONG lastLoggedAt = 0;
    const auto now = GetTickCount64();
    if (*s && message != s && (lastLogged != s || now - lastLoggedAt >= 5000) &&
        Config::Instance()->LogToFile.value_or_default())
    {
        lastLogged = s; lastLoggedAt = now;
        DlssNr::Diagnostics::Append(Util::DllPath().parent_path() / L"amd_bridge.log",
            std::to_string(GetTickCount64()) + " thread=" + std::to_string(GetCurrentThreadId()) + " " + s);
    }
    message = s;
}
thread_local NVSDK_NGX_Parameter* replacedParams = nullptr;
thread_local ID3D12Resource* originalColour = nullptr;
struct FrameIdentity
{
    ID3D12Resource* colour = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* depth = nullptr;
    UINT width = 0, height = 0;
};
std::mutex frameMutex;
FrameIdentity lastFrame {};
UINT stableFrames = 0;
thread_local uint64_t submitOrdinal = 0; // Monotonic per-thread submission counter.

class ConfirmedQueueHolder
{
    mutable std::mutex mu_;
    ID3D12CommandQueue* queue_ = nullptr;
public:
    ~ConfirmedQueueHolder()
    {
        Clear();
    }
    void Clear()
    {
        std::lock_guard lock(mu_);
        if (queue_)
        {
            queue_->Release();
            queue_ = nullptr;
        }
    }
    void Set(ID3D12CommandQueue* q)
    {
        std::lock_guard lock(mu_);
        if (queue_ == q)
            return;
        if (q)
            q->AddRef();
        if (queue_)
            queue_->Release();
        queue_ = q;
    }
    ID3D12CommandQueue* Get() const
    {
        std::lock_guard lock(mu_);
        if (queue_)
            queue_->AddRef();
        return queue_; // Caller must Release()
    }
    bool Matches(ID3D12CommandQueue* q) const
    {
        std::lock_guard lock(mu_);
        if (!queue_ || !q)
            return queue_ == q;
        if (queue_ == q)
            return true;
        IUnknown* id1 = nullptr;
        IUnknown* id2 = nullptr;
        queue_->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&id1));
        q->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&id2));
        const bool same = (id1 && id2 && id1 == id2);
        if (id1) id1->Release();
        if (id2) id2->Release();
        return same;
    }
};

static ConfirmedQueueHolder s_confirmedRenderQueue;
static AwaitingListTracker s_awaitingTracker;

void SetConfirmedRenderQueueInternal(ID3D12CommandQueue *q)
{
    s_confirmedRenderQueue.Set(q);
}

void ExecuteBatch(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* c)
{
    if (q && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        ID3D12GraphicsCommandList *matched = s_awaitingTracker.MatchAndRemove(n, c);
        if (matched)
        {
            s_confirmedRenderQueue.Set(q);
            LOG_INFO("AMD NR: confirmed execution queue {:p} for target list {:p}",
                     reinterpret_cast<void*>(q), reinterpret_cast<void*>(matched));
        }
    }
    // Notify every live host. Each no-ops on lists it does not own, so a switch
    // between Record and Execute still lands Submitted on the recording host.
    auto daniel = g_daniel.load(std::memory_order_acquire);
    auto lmxxf = g_lmxxf.load(std::memory_order_acquire);
    if (daniel)
        daniel->Submitting(q, n, c);
    if (lmxxf)
        lmxxf->Submitting(q, n, c);
    if (auto b = g_mochizuki.load()) b->Submitting(q, n, c);
    // Execute every game list exactly once. Private runtime Notify callbacks
    // publish HIP jobs afterwards and have their internal ECL call neutralized.
    // When lmxxf submission expand is armed, unwrap CommandListProxy (between = HIP slot).
    // Expand runs for every ExecuteBatch when ExpandEnabled ? independent of PendingListIndex
    // (Daniel-only isolation of a private neural list).
    if (DlssNr::Submission::Hooks::ExpandEnabled())
    {
        const auto between = DlssNr::Submission::Hooks::GetBetween();
        DlssNr::Submission::Hooks::ExecuteExpanded(q, n, c, between.fn, between.ctx, executeOriginal);
    }
    else
        executeOriginal(q, n, c);
    {
        std::lock_guard guard(observedMutex);
        if (observedLists.size() > 256)
            observedLists.clear();
        for (UINT i = 0; i < n; ++i)
            observedLists.insert(c[i]);
    }
    if (daniel)
        daniel->Submitted(q, n, c);
    if (lmxxf)
        lmxxf->Submitted(q, n, c);
    if (auto b = g_mochizuki.load()) b->Submitted(q, n, c);
}
void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* c)
{
    // Only the active host may split the batch. A stale Daniel slot after
    // switching to lmxxf must not isolate lists the lmxxf path submits whole.
    auto b = ActiveHost();
    int index = b ? b->PendingListIndex(n, c) : -1;
    if (n > 1 && index >= 0)
    {
        // Separate Execute calls establish an execution boundary around the
        // interop list. Preserve list order and execute each list exactly once.
        // Before() clears the status each frame, defeating Message's adjacent
        // duplicate check. This routine event needs only one log per process,
        // including when different submission threads reach it concurrently.
        static std::atomic_flag isolationLogged = ATOMIC_FLAG_INIT;
        if (!isolationLogged.test_and_set(std::memory_order_relaxed))
            Message("AMD isolated neural command list from a render batch (logged once)");
        if (index) ExecuteBatch(q, static_cast<UINT>(index), c);
        ExecuteBatch(q, 1, c + index);
        auto remaining = n - static_cast<UINT>(index) - 1;
        if (remaining) ExecuteBatch(q, remaining, c + index + 1);
        return;
    }
    ExecuteBatch(q, n, c);
}
void NTAPI Exit(LONG code)
{
    s_confirmedRenderQueue.Clear();
    s_awaitingTracker.Clear();
    if (auto b = g_daniel.load())
        b->Shutdown();
    if (auto b = g_lmxxf.load())
        b->Shutdown();
    if (auto b = g_mochizuki.load())
        b->Shutdown();
    exitOriginal(code);
}
// Caller holds initMutex. Install once, before any proxy can escape into game submission.
bool InstallSubmissionHook(ID3D12Device *device, ID3D12CommandQueue *q)
{
    if (submissionHookReady)
        return true;
    executeOriginal = reinterpret_cast<ExecuteFn>((*reinterpret_cast<void***>(q))[10]);
    // FG can expose a proxy present queue. Hook the device's execution
    // implementation so actual render submissions are still observed.
    ID3D12CommandQueue* probe = nullptr;
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    if (SUCCEEDED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&probe))))
    {
        executeOriginal = reinterpret_cast<ExecuteFn>((*reinterpret_cast<void***>(probe))[10]);
        probe->Release();
    }
    exitOriginal = reinterpret_cast<ExitFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlExitUserProcess"));
    LONG err = DetourTransactionBegin();
    if (err == NO_ERROR)
        err = DetourUpdateThread(GetCurrentThread());
    if (err == NO_ERROR)
        err = DetourAttach(reinterpret_cast<PVOID*>(&executeOriginal), Execute);
    if (err == NO_ERROR && exitOriginal)
        err = DetourAttach(reinterpret_cast<PVOID*>(&exitOriginal), Exit);
    if (err == NO_ERROR)
        err = DetourTransactionCommit();
    else
        DetourTransactionAbort();
    if (err != NO_ERROR)
    {
        Message("AMD NR: could not install submission notification");
        return false;
    }
    DlssNr::Submission::NoteRawExecuteCommandLists(executeOriginal);
    submissionHookReady = true;
    return true;
}
std::filesystem::path Directory() { return Util::DllPath().parent_path(); }
ID3D12Resource* Resource(NVSDK_NGX_Parameter* p, const char* name)
{
    ID3D12Resource* r = nullptr;
    if (p->Get(name, &r) != NVSDK_NGX_Result_Success)
        p->Get(name, reinterpret_cast<void**>(&r));
    return r;
}
bool IsAmd(ID3D12Device* d)
{
    struct PhysicalAdapterScope
    {
        uint64_t id = SkipSpoof::AddEntry(SkipSpoofType::Thread);
        ~PhysicalAdapterScope() { SkipSpoof::RemoveEntry(id); }
    } physicalAdapterScope;
    IDXGIFactory4* f = nullptr;
    IDXGIAdapter1* a = nullptr;
    bool amd = false;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))
    {
        if (SUCCEEDED(f->EnumAdapterByLuid(d->GetAdapterLuid(), IID_PPV_ARGS(&a))))
        {
            DXGI_ADAPTER_DESC1 desc {};
            amd = SUCCEEDED(a->GetDesc1(&desc)) && desc.VendorId == 0x1002;
            LOG_INFO("AMD NR physical adapter vendor: {:04X}, AMD: {}", desc.VendorId, amd);
            a->Release();
        }
        f->Release();
    }
    return amd;
}
} // namespace
void UpdateConfirmedRenderQueue(ID3D12CommandQueue *q)
{
    SetConfirmedRenderQueueInternal(q);
}
bool EnsureSubmissionHook(ID3D12CommandQueue *q)
{
    if (!q)
        return false;
    std::lock_guard lock(initMutex);
    if (submissionHookReady)
        return true;
    ID3D12Device *device = nullptr;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&device))))
        return false;
    const bool ready = InstallSubmissionHook(device, q);
    device->Release();
    return ready;
}
bool HasDanielRuntime()
{
    return DlssNr::Backend::HasDanielInstalled();
}
bool HasLmxxfRuntime()
{
    return DlssNr::Backend::HasLmxxfInstalled();
}
bool HasFiles()
{
    // Proxy names such as winmm.dll can load before Util::DllPath is finalized.
    // A negative result cached at that point disabled the AMD backend for the
    // rest of the process. If the selected runtime is absent, allow the cached
    // choice to follow a later successful probe at the real package path.
    auto active = ActiveKindCached();
    auto installed = [](DlssNr::Backend::Kind kind) {
        if (kind == DlssNr::Backend::Kind::Mochizuki) return DlssNr::Backend::HasMochizukiInstalled();
        return kind == DlssNr::Backend::Kind::Lmxxf ? HasLmxxfRuntime() : HasDanielRuntime();
    };
    if (installed(active))
        return true;
    const auto resolved = DlssNr::Backend::ActiveKindFromConfig();
    int expected = static_cast<int>(active);
    g_activeKind.compare_exchange_strong(expected, static_cast<int>(resolved),
                                         std::memory_order_acq_rel);
    active = ActiveKindCached();
    return installed(active);
}
void SyncBackendWithConfig()
{
    DlssNr::Effects::Reset();
    // Hot switch: both hosts stay alive. Drop temporal history and force the
    // warm-up window so the new host does not inherit stability.
    DlssNr::Backend::InvalidateInstallProbe();
    const auto selected = DlssNr::Backend::ActiveKindFromConfig();
    const auto previous = ActiveKindCached();
    if (previous != selected)
        if (auto old = ActiveHost()) old->ReleaseSession();
    g_activeKind.store(static_cast<int>(selected), std::memory_order_release);
    {
        std::lock_guard fl(frameMutex);
        lastFrame = {};
        stableFrames = 0;
    }
    // Sticky on: never clear ProxyWrap. Lists created while it was off stay raw
    // forever, so daniel -> lmxxf would fail the same-frame QI. Once wrapping is
    // on, leaving it on costs only a thin CPU proxy during daniel.
    if (DlssNr::Submission::Hooks::IsArmed() && DlssNr::Backend::ProxyWrapWanted())
        DlssNr::Submission::Hooks::SetProxyWrap(true);
    if (auto b = ActiveHost())
    {
        b->InvalidateHistory();
        // A daniel host that was just selected (or re-enabled after lmxxf) must
        // be allowed to build its graphics PSO instead of staying compute-first.
        if (selected == DlssNr::Backend::Kind::Daniel)
            b->ResetGraphicsWaitState();
    }
    Message("AMD NR: NR backend switched");
}
bool IsRunning()
{
    const auto host = ActiveHost();
    return host && host->IsRunning();
}
const char* RuntimeName()
{
    // The menu queries this every frame. Cache both known and unknown hashes,
    // but recheck the path and metadata so an early proxy-path query or a DLL
    // replacement does not leave a stale display for the rest of the process.
    // Runtime loading still performs its own full SHA validation.
    static std::mutex cacheMutex;
    static std::filesystem::path cachedPath;
    static std::uintmax_t cachedSize = 0;
    static std::filesystem::file_time_type cachedWriteTime {};
    static const char* cachedName = nullptr;
    static bool cached = false;
    std::lock_guard lock(cacheMutex);
    std::error_code ec;
    const auto path = Directory() / L"dlssnr_amd_pass1.dll";
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        cached = false;
        return nullptr;
    }
    const auto writeTime = std::filesystem::last_write_time(path, ec);
    if (ec)
    {
        cached = false;
        return nullptr;
    }
    if (!cached || path != cachedPath || size != cachedSize || writeTime != cachedWriteTime)
    {
        cachedName = AmdPreSr::IdentifyRuntimeName(path);
        cachedPath = path;
        cachedSize = size;
        cachedWriteTime = writeTime;
        cached = true;
    }
    return cachedName;
}
bool Evaluate(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* params, ID3D12CommandQueue* q, bool beforeUpscale)
{
    // A single backend consumes one SR stream even if the engine rotates worker threads.
    // Serialize shared settling/identity state; thread-local replacement ownership stays unchanged.
    std::lock_guard frameGuard(frameMutex);
    bool effectRecorded = false;
    struct EffectHistoryGuard {
        bool& recorded;
        ~EffectHistoryGuard() { if (!recorded) DlssNr::Effects::InvalidateHistory(); }
    } effectHistoryGuard {effectRecorded};
    DlssNr::Backend::LmxxfProbe::CurrentEvidence() = {};
    if (!HasFiles())
        return false;
    const auto requested = DlssNr::Backend::RequestedKind();
    const auto active = ActiveKindCached();
    if (requested == DlssNr::Backend::Kind::Lmxxf && !DlssNr::Backend::LmxxfWired())
    {
        static bool loggedLmxxfFallback = false;
        if (!loggedLmxxfFallback)
        {
            loggedLmxxfFallback = true;
            Message("AMD NR: NrBackend=lmxxf is not wired; using daniel");
        }
    }
    ID3D12Device* device = nullptr;
    if (!cmd || !params || FAILED(cmd->GetDevice(IID_PPV_ARGS(&device))))
        return true;
    thread_local LUID checkedAdapter {};
    thread_local bool checked = false, amd = false;
    const auto adapter = device->GetAdapterLuid();
    if (!checked || adapter.HighPart != checkedAdapter.HighPart || adapter.LowPart != checkedAdapter.LowPart)
    {
        amd = IsAmd(device);
        checkedAdapter = adapter;
        checked = true;
    }
    if (!amd)
    {
        device->Release();
        return false;
    }
    ID3D12CommandQueue* confirmedQ = nullptr;
    if (!q)
    {
        confirmedQ = s_confirmedRenderQueue.Get();
        if (confirmedQ)
        {
            ID3D12Device* qDev = nullptr;
            if (SUCCEEDED(confirmedQ->GetDevice(IID_PPV_ARGS(&qDev))))
            {
                IUnknown* devId1 = nullptr;
                IUnknown* devId2 = nullptr;
                device->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&devId1));
                qDev->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&devId2));
                if (devId1 && devId2 && devId1 == devId2)
                {
                    q = confirmedQ;
                }
                if (devId1) devId1->Release();
                if (devId2) devId2->Release();
                qDev->Release();
            }
            if (!q)
            {
                confirmedQ->Release();
                confirmedQ = nullptr;
            }
        }
    }
    if (!q)
    {
        // Target list not yet observed on any execution queue.
        // Register cmd as awaiting observation, and bypass NR this frame (original Color to SR).
        s_awaitingTracker.Add(cmd);
        if (!submissionHookReady)
        {
            auto *fallback = reinterpret_cast<ID3D12CommandQueue*>(State::Instance().currentCommandQueue);
            if (fallback)
                InstallSubmissionHook(device, fallback);
        }
        device->Release();
        static bool s_loggedWait = false;
        if (!s_loggedWait)
        {
            s_loggedWait = true;
            LOG_INFO("AMD NR: awaiting execution queue observation for target command list {:p}; bypassing NR this frame",
                     reinterpret_cast<void*>(cmd));
        }
        return true;
    }
    std::lock_guard initGuard(initMutex);
    if (!InstallSubmissionHook(device, q))
    {
        device->Release();
        if (confirmedQ) confirmedQ->Release();
        return true;
    }
    // Sticky on (see SyncBackendWithConfig): never clear. Convenience mode wraps
    // even for a daniel host so a later switch to lmxxf keeps the same-frame QI.
    if (DlssNr::Submission::Hooks::IsArmed() && DlssNr::Backend::ProxyWrapWanted())
        DlssNr::Submission::Hooks::SetProxyWrap(true);
    // Build only the selected host on first use. The other is built when it is
    // first selected (switch). Hosts persist; inactive sessions request release.
    if (active == DlssNr::Backend::Kind::Mochizuki)
    {
        if (!g_mochizuki.load(std::memory_order_acquire))
            g_mochizuki.store(new DlssNr::Backend::MochizukiBackend(device, q, Directory()), std::memory_order_release);
    }
    else if (active == DlssNr::Backend::Kind::Lmxxf)
    {
        if (!g_lmxxf.load(std::memory_order_acquire))
            g_lmxxf.store(new DlssNr::Backend::LmxxfBackend(device, q, Directory()),
                          std::memory_order_release);
    }
    else if (!g_daniel.load(std::memory_order_acquire))
    {
        g_daniel.store(new DlssNr::Backend::DanielBackend(device, q, Directory()),
                       std::memory_order_release);
    }
    auto b = HostForKind(active);
    device->Release();
    if (confirmedQ)
    {
        confirmedQ->Release();
        confirmedQ = nullptr;
    }
    // The hook observes this list when the current frame is submitted and then
    // binds the actual queue before waking HIP. Engines that rotate command-list
    // objects may never submit the same object twice, so do not require a prior
    // observation here.
    Message("");
    static bool lastBefore = true;
    if (lastBefore != beforeUpscale)
    {
        lastBefore = beforeUpscale;
        lastFrame = {}; stableFrames = 0;
        b->InvalidateHistory();
        DlssNr::Effects::InvalidateHistory();
        DlssNr::PostSr::Reset();
        LOG_INFO("AMD NR order: {}", beforeUpscale ? "NR -> SR" : "SR -> NR (experimental)");
    }
    // The swapchain's present queue can change when FG is enabled. It is
    // only a bootstrap hint; Submitted identifies the queue executing our list.
    AmdPreSr::Frame f {};
    f.colour = Resource(params, beforeUpscale ? NVSDK_NGX_Parameter_Color : NVSDK_NGX_Parameter_Output);
    f.motion = Resource(params, NVSDK_NGX_Parameter_MotionVectors);
    f.depth = Resource(params, NVSDK_NGX_Parameter_Depth);
    f.exposure = Resource(params, NVSDK_NGX_Parameter_ExposureTexture);
    params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &f.preExposure);
    params->Get(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, &f.exposureScale);
    const auto renderWidthResult = params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &f.width);
    const auto renderHeightResult = params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &f.height);
    const UINT rawRenderWidth = f.width, rawRenderHeight = f.height;
    UINT renderWidth = f.width, renderHeight = f.height;
    if (!beforeUpscale)
    {
        // Render subrect describes the guides, never the completed SR colour.
        if (auto input = Resource(params, NVSDK_NGX_Parameter_Color))
        {
            if (!renderWidth) renderWidth = UINT(input->GetDesc().Width);
            if (!renderHeight) renderHeight = input->GetDesc().Height;
        }
        f.width = f.height = 0;
        params->Get(NVSDK_NGX_Parameter_OutWidth, &f.width);
        params->Get(NVSDK_NGX_Parameter_OutHeight, &f.height);
    }
    if (f.colour)
    {
        const auto extent = f.colour->GetDesc();
        if (!f.width) f.width = static_cast<UINT>(extent.Width);
        if (!f.height) f.height = extent.Height;
    }
    UINT x = 0, y = 0, flags = 0, reset = 0;
    params->Get(beforeUpscale ? NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X : NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, &x);
    params->Get(beforeUpscale ? NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y : NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, &y);
    if (x || y)
    {
        Message("AMD NR: nonzero colour subrect origin unsupported");
        return true;
    }
    if (!beforeUpscale)
    {
        for (const char* key : {NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                               NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y,
                               NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                               NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y})
        {
            UINT origin = 0; params->Get(key, &origin);
            if (origin) { Message("SR -> NR: nonzero guide subrect origin unsupported"); return true; }
        }
    }
    auto haveFlags = params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags) == NVSDK_NGX_Result_Success;
    if (haveFlags && !(flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) && f.motion)
    {
        params->Get(NVSDK_NGX_Parameter_OutWidth, &f.motionWidth);
        params->Get(NVSDK_NGX_Parameter_OutHeight, &f.motionHeight);
        if (!f.motionWidth) f.motionWidth = static_cast<UINT>(f.motion->GetDesc().Width);
        if (!f.motionHeight) f.motionHeight = f.motion->GetDesc().Height;
    }
    // Mochizuki's DRS buckets must see every render subrect, including changing
    // extents. Its runtime owns rebuilds and history invalidation for these.
    const bool runtimeDrs = active == DlssNr::Backend::Kind::Mochizuki &&
        Config::Instance()->MochizukiDynamicResolution.value_or_default() != 0;
    // Otherwise let SR finish reconfiguration before rebuilding the model.
    // Do not retain or replay the old image while input sizes are settling.
    static UINT settlingWidth=0, settlingHeight=0;
    static float settlingScale=1.f;
    static ULONGLONG settlingSince=0;
    const float sessionScale = active == DlssNr::Backend::Kind::Mochizuki ?
        Config::Instance()->MochizukiModelScale.value_or_default() :
        Config::Instance()->AmdNrScale.value_or_default();
    const float requestedScale=sessionScale;
    const auto now=GetTickCount64();
    const bool firstProbe = (settlingWidth == 0 && settlingHeight == 0);
    if(settlingWidth!=f.width || settlingHeight!=f.height || settlingScale!=requestedScale) {
        if (active == DlssNr::Backend::Kind::Mochizuki)
        {
            static unsigned extentChanges = 0;
            if (++extentChanges <= 8 || extentChanges % 100 == 0)
            {
                const auto cd = f.colour ? f.colour->GetDesc() : D3D12_RESOURCE_DESC{};
                const auto md = f.motion ? f.motion->GetDesc() : D3D12_RESOURCE_DESC{};
                LOG_INFO("Mochi input #{}: {} -> {}x{}, raw render {}x{} (Get {:x}/{:x}), colour {:p} "
                         "allocation {}x{} DXGI {}, motion {}x{} DXGI {}, params {:p}, DRS {}",
                         extentChanges, beforeUpscale ? "pre-SR" : "post-SR", f.width, f.height,
                         rawRenderWidth, rawRenderHeight, unsigned(renderWidthResult), unsigned(renderHeightResult),
                         static_cast<void*>(f.colour), cd.Width, cd.Height, unsigned(cd.Format),
                         md.Width, md.Height, unsigned(md.Format), static_cast<void*>(params),
                         Config::Instance()->MochizukiDynamicResolution.value_or_default());
            }
        }
        if (!firstProbe && !runtimeDrs)
        {
            // Real change after we already had a size: keep the settle window.
            b->TraceBoundary("settings change: input " + std::to_string(settlingWidth) + "x" +
                std::to_string(settlingHeight) + " -> " + std::to_string(f.width) + "x" +
                std::to_string(f.height) + "; NR scale " + std::to_string(settlingScale) +
                " -> " + std::to_string(requestedScale));
            // Dynamic resolution can change the extent every few frames; keep the log bounded.
            static unsigned settleChanges = 0;
            ++settleChanges;
            if (settleChanges <= 8 || settleChanges % 100 == 0)
                LOG_INFO("AMD NR settle #{}: {}x{} scale {:.3f} -> {}x{} scale {:.3f} (thread {})",
                         settleChanges, settlingWidth, settlingHeight, settlingScale, f.width, f.height,
                         requestedScale, GetCurrentThreadId());
            b->InvalidateHistory();
            settlingSince=now;
        }
        // First probe (0x0 -> real size) is startup, not a mid-session change.
        // Leave settlingSince at 0 so we do not skip the first stable frames.
        settlingWidth=f.width;settlingHeight=f.height;settlingScale=requestedScale;
    }
    if (runtimeDrs) settlingSince = 0;
    if(settlingSince != 0 && now-settlingSince<300) {
        // Once per settle window, not every 250 ms: Message() also lands in amd_bridge.log.
        static ULONGLONG loggedWindow = 0;
        if (loggedWindow != settlingSince)
        {
            loggedWindow = settlingSince;
            LOG_INFO("AMD neural: waiting for resolution settings to settle ({}x{} scale {:.3f}, thread {})",
                     f.width, f.height, requestedScale, GetCurrentThreadId());
            Message("AMD neural: waiting for resolution settings to settle");
        }
        return true;
    }
    const FrameIdentity current { f.colour, f.motion, f.depth, f.width, f.height };
    // Resource addresses rotate in Unreal's frame buffers. Only an extent
    // change requires warm-up; pointer equality can suppress every frame.
    const bool sameFrame = runtimeDrs || (current.width == lastFrame.width &&
                                         current.height == lastFrame.height);
    if (!sameFrame)
    {
        lastFrame = current;
        stableFrames = 0;
        b->InvalidateHistory();
        Message("AMD NR: warming up after an upscaler/resource change");
        return true;
    }
    lastFrame = current;
    if (runtimeDrs) stableFrames = 2;
    if (stableFrames < 2 && ++stableFrames < 2)
    {
        Message("AMD NR: warming up after an upscaler/resource change");
        return true;
    }
    f.depthInverted = (flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    params->Get(NVSDK_NGX_Parameter_Reset, &reset);
    f.reset = reset != 0;
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &f.jitterX);
    params->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &f.jitterY);
    f.motionJittered = (flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
    params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &f.motionScaleX);
    params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &f.motionScaleY);
    const auto& cfg = *Config::Instance();
    if (cfg.ColorResourceBarrier.has_value())
        f.colourState = static_cast<D3D12_RESOURCE_STATES>(cfg.ColorResourceBarrier.value());
    if (!beforeUpscale)
        f.colourState = cfg.OutputResourceBarrier.has_value() ?
            static_cast<D3D12_RESOURCE_STATES>(cfg.OutputResourceBarrier.value()) : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (cfg.MVResourceBarrier.has_value())
        f.motionState = static_cast<D3D12_RESOURCE_STATES>(cfg.MVResourceBarrier.value());
    if (cfg.DepthResourceBarrier.has_value())
        f.depthState = static_cast<D3D12_RESOURCE_STATES>(cfg.DepthResourceBarrier.value());
    if (cfg.ExposureResourceBarrier.has_value())
        f.exposureState = static_cast<D3D12_RESOURCE_STATES>(cfg.ExposureResourceBarrier.value());
    AmdPreSr::Settings s {};
    s.rtgi.enabled = cfg.AmdRtgiEnabled.value_or_default();
    s.rtgi.quality = cfg.AmdRtgiQuality.value_or_default();
    s.rtgi.denoiser = cfg.AmdRtgiDenoiser.value_or_default();
    s.rtgi.inspect = cfg.AmdRtgiInspect.value_or_default();
    s.rtgi.contact = cfg.AmdRtgiContact.value_or_default();
    s.rtgi.saturation = cfg.AmdRtgiSaturation.value_or_default();
    s.rtgi.radius = cfg.AmdRtgiRadius.value_or_default();
    s.rtgi.mix = cfg.AmdRtgiMix.value_or_default();
    s.rtgi.lighting = cfg.AmdRtgiLighting.value_or_default();
    s.rtgi.occlusion = cfg.AmdRtgiOcclusion.value_or_default();
    s.rtgi.ambient = cfg.AmdRtgiAmbient.value_or_default();
    s.rtgi.thickness = cfg.AmdRtgiThickness.value_or_default();
    s.rtgi.smoothness = cfg.AmdRtgiSmoothness.value_or_default();
    s.rtgi.fade = cfg.AmdRtgiFade.value_or_default();
    s.rtgi.fov = cfg.AmdRtgiFov.value_or_default();
    s.rtgi.farPlane = cfg.AmdRtgiFarPlane.value_or_default();
    s.look.enabled = cfg.AmdLookEnabled.value_or_default();
    s.look.appearance = 2; // Single default profile; ignore legacy preset selections.
    s.look.mix = cfg.AmdLookMix.value_or_default();
    s.look.materialDetail = cfg.AmdLookMaterialDetail.value_or_default();
    s.look.shapeDefinition = cfg.AmdLookShapeDefinition.value_or_default();
    s.look.localLighting = cfg.AmdLookLocalLighting.value_or_default();
    s.look.skinDetail = cfg.AmdLookSkinDetail.value_or_default();
    s.look.skinSoftness = cfg.AmdLookSkinSoftness.value_or_default();
    s.look.detectSkin = cfg.AmdLookDetectSkin.value_or_default();
    s.look.specularControl = cfg.AmdLookSpecularControl.value_or_default();
    s.look.highlightRollOff = cfg.AmdLookHighlightRollOff.value_or_default();
    s.look.colourSeparation = cfg.AmdLookColourSeparation.value_or_default();
    s.look.shadowDepth = cfg.AmdLookShadowDepth.value_or_default();
    s.look.antiHalo = cfg.AmdLookAntiHalo.value_or_default();
    s.look.flatAreaProtection = cfg.AmdLookFlatAreaProtection.value_or_default();
    s.look.inspect = cfg.AmdLookInspect.value_or_default();
    s.look.tone = cfg.AmdLookTone.value_or_default();
    s.look.exposureEV = cfg.AmdLookExposureEV.value_or_default();
    s.look.contrast = cfg.AmdLookContrast.value_or_default();
    s.look.saturation = cfg.AmdLookSaturation.value_or_default();
    s.look.highlightCompression = cfg.AmdLookHighlightCompression.value_or_default();
    s.modelScale = sessionScale;
    s.passes = cfg.DlssNrPasses.value_or_default();
    s.everyFrame = cfg.AmdEveryFrame.value_or_default();
    s.slots = std::clamp(cfg.AmdSlots.value_or_default(), 1, 5);
    // AmdGraphicsWait=1 requests 0.3.1's 1-pixel draw wait (this project's New wait).
    // InitPass/Record still force SpinDraw=0 unless a freeze+restore plan armed.
    s.spinDraw = Config::Instance()->AmdGraphicsWait.value_or_default() ? 1 : 0;
    s.encoding=std::clamp(cfg.AmdEncoding.value_or_default(),0,3);
    s.toneChannels = cfg.AmdToneChannels.value_or(cfg.AmdNeuralLightingStrength.value_or_default() > 0);
    s.autoMask = cfg.DlssNrAutoMask.value_or_default();
    s.useGameExposure = cfg.AmdUseGameExposure.value_or_default();
    s.style = (std::min)(cfg.DlssNrStyle.value_or_default(), 2u);
    s.toneCurve = cfg.DlssNrToneCurve.value_or_default() ? 1u : 0u;
    s.toneLift = AmdPreSr::BoundedToneLift(cfg.DlssNrToneLift.value_or_default());
    s.tone = std::clamp(cfg.AmdNeuralLightingStrength.value_or_default(), 0.f, 1.f);
    s.structure = cfg.DlssNrLocalStructure.value_or_default();
    s.skin = cfg.DlssNrSkinStructure.value_or_default();
    if (s.skin < 0)
        s.skin = s.structure;
    // Evaluate cut: Split proxy + SetBetween(EnqueueHip) is owned by lmxxf Record.
    DlssNr::Backend::LmxxfCut::OnEvaluateBeforeRecord(cmd);
    std::lock_guard effectsLifetime(DlssNr::Submission::RecordingMutex());
    std::shared_ptr<DlssNr::PostSr::Lease> post;
    if (!beforeUpscale)
    {
        // Rebuild temporal state if the guide grid changes under a fixed SR output.
        static UINT lastRenderWidth = 0, lastRenderHeight = 0, lastMotionWidth = 0, lastMotionHeight = 0;
        f.reset |= renderWidth != lastRenderWidth || renderHeight != lastRenderHeight ||
                   f.motionWidth != lastMotionWidth || f.motionHeight != lastMotionHeight;
        lastRenderWidth = renderWidth; lastRenderHeight = renderHeight;
        lastMotionWidth = f.motionWidth; lastMotionHeight = f.motionHeight;
        // Jittered MV cannot be used as unjittered post-SR history without a
        // previous-jitter contract. Keep this experimental route spatial there.
        f.reset |= f.motionJittered;
        std::string reason;
        post = DlssNr::PostSr::Prepare(cmd, f, renderWidth, renderHeight, reason);
        if (!post) { b->InvalidateHistory(); Message(reason.c_str()); return true; }
    }
    if (auto replacement = b->Record(cmd, f, s))
    {
        effectRecorded = true;
        replacement = DlssNr::Effects::Record(cmd, f.colour, replacement, f.colourState, f.width, f.height,
            cfg.NrOverallIntensity.value_or_default(), cfg.NrTimingEnabled.value_or_default(),
            {f.motion, f.depth, f.motionState, f.depthState,
             f.motionWidth ? f.motionWidth : f.width, f.motionHeight ? f.motionHeight : f.height,
             f.motionScaleX, f.motionScaleY, f.jitterX, f.jitterY, f.preExposure, f.exposureScale,
             f.depthInverted, f.motionJittered, f.reset},
            {cfg.NrStabilizerEnabled.value_or_default(), cfg.NrStabilizerAlpha.value_or_default(),
             cfg.NrStabilizerThreshold.value_or_default()});
        if (beforeUpscale)
        {
            originalColour = f.colour;
            replacedParams = params;
            params->Set(NVSDK_NGX_Parameter_Color, replacement);
        }
        else if (!DlssNr::PostSr::Finish(cmd, post, replacement, f.colourState))
            Message("SR -> NR: unsupported NR result; keeping the SR output");
    }
    else DlssNr::Effects::InvalidateHistory();
    return true;
}
bool HasReplacement(NVSDK_NGX_Parameter* params)
{
    return params && replacedParams == params && originalColour;
}
void Restore(NVSDK_NGX_Parameter* params)
{
    if (params && replacedParams == params)
    {
        params->Set(NVSDK_NGX_Parameter_Color, originalColour);
        replacedParams = nullptr;
        originalColour = nullptr;
    }
}
void InvalidateHistory()
{
    DlssNr::Effects::InvalidateHistory();
    if (auto b = ActiveHost())
        b->InvalidateHistory();
}
void PollReleases()
{
    DlssNr::PostSr::Poll();
    DlssNr::Effects::Poll();
    if (auto b = g_daniel.load(std::memory_order_acquire)) b->PollRelease();
    if (auto b = g_lmxxf.load(std::memory_order_acquire)) b->PollRelease();
    if (auto b = g_mochizuki.load(std::memory_order_acquire)) b->PollRelease();
}
void OnNrDisabled()
{
    DlssNr::PostSr::Reset();
    DlssNr::Effects::Reset();
    // Both hosts are released so the inactive one is not left holding VRAM either.
    if (auto b = g_daniel.load(std::memory_order_acquire))
    {
        b->ReleaseSession();
        b->ResetGraphicsWaitState();
    }
    if (auto b = g_lmxxf.load(std::memory_order_acquire))
        b->ReleaseSession();
    if (auto b = g_mochizuki.load(std::memory_order_acquire)) b->ReleaseSession();
}
void TraceContextRelease(unsigned int handle, bool after)
{
    if (auto b = ActiveHost())
        b->TraceBoundary(std::string(after ? "after" : "before") +
                         " SR context release handle=" + std::to_string(handle));
}
bool GraphicsRestartNeeded(UINT activePasses)
{
    if (auto b = ActiveHost())
        return b->GraphicsRestartNeeded(activePasses);
    return false;
}
NrTimingSnapshot Timing()
{
    // This read must not discover files or instantiate a backend from the UI.
    if (!Config::Instance()->DlssNrEnabled.value_or_default() ||
        !Config::Instance()->NrTimingEnabled.value_or_default()) return {};
    const int kind = g_activeKind.load(std::memory_order_acquire);
    if (kind < 0) return {};
    NrTimingSnapshot snapshot {};
    if (auto* host = HostForKind(static_cast<DlssNr::Backend::Kind>(kind))) snapshot = host->Timing();
    const auto effects = DlssNr::Effects::Timing();
    if (effects.enabled) {
        if (snapshot.version != NR_TIMING_VERSION) {
            snapshot.struct_size = sizeof snapshot; snapshot.version = NR_TIMING_VERSION; snapshot.enabled = 1;
        }
        snapshot.stages[NR_GPU_BLEND] = effects.stages[NR_GPU_BLEND];
        snapshot.stages[NR_GPU_STABILIZER] = effects.stages[NR_GPU_STABILIZER];
        snapshot.dropped += effects.dropped;
    }
    return snapshot;
}
MochizukiNrBuildProgress BuildProgress()
{
    if (!Config::Instance()->DlssNrEnabled.value_or_default() ||
        g_activeKind.load(std::memory_order_acquire) != int(DlssNr::Backend::Kind::Mochizuki)) return {};
    if (auto* host = g_mochizuki.load(std::memory_order_acquire)) return host->BuildProgress();
    return {};
}
std::string EffectsStatus() { return DlssNr::Effects::Status(); }
std::string Status()
{
    if(AmdPresentExperimental::IsTarget()) return AmdPresentExperimental::Status();
    {
        std::lock_guard l(messageMutex);
        if (!message.empty())
            return message;
    }
    if (auto b = ActiveHost())
        return b->Status();
    return "AMD NR: idle";
}
} // namespace DlssNr::AmdBridge
