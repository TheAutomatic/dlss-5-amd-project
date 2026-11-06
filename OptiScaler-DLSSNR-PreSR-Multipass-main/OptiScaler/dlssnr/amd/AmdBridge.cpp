#include "pch.h"
#include "AmdBridge.h"
#include "../AsiTakeover.h"
#include "AwaitingListTracker.h"
#include "../backend/Selector.h"
#include "../submission/SubmissionHooks.h"
#include "../NrGameWaitDiag.h"
#include <Config.h>
#include <detours/detours.h>
#include <mutex>

namespace DlssNr::AmdBridge
{
namespace
{
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using ExitFn = void(NTAPI*)(LONG);
ExecuteFn executeOriginal = nullptr;
ExitFn exitOriginal = nullptr;
bool submissionHookReady = false; // guarded by initMutex
std::mutex initMutex;

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
};

static ConfirmedQueueHolder s_confirmedRenderQueue;
static AwaitingListTracker s_awaitingTracker;

void SetConfirmedRenderQueueInternal(ID3D12CommandQueue* q)
{
    s_confirmedRenderQueue.Set(q);
}

void ExecuteBatch(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* c)
{
    if (q && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        ID3D12GraphicsCommandList* matched = s_awaitingTracker.MatchAndRemove(n, c);
        if (matched)
        {
            s_confirmedRenderQueue.Set(q);
            LOG_INFO("AMD NR: confirmed execution queue {:p} for target list {:p}",
                     reinterpret_cast<void*>(q), reinterpret_cast<void*>(matched));
        }
    }
    if (DlssNr::Submission::Hooks::ExpandEnabled())
    {
        const auto between = DlssNr::Submission::Hooks::GetBetween();
        DlssNr::Submission::Hooks::ExecuteExpanded(q, n, c, between.fn, between.ctx, executeOriginal);
    }
    else
        executeOriginal(q, n, c);
    DlssNr::Submission::GpuBatch::Tracker::Instance().OnExecuted(n, c, q);
}

void STDMETHODCALLTYPE Execute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* c)
{
    DlssNr::GameWait::OnExecute(q);
    ExecuteBatch(q, n, c);
}

void NTAPI Exit(LONG code)
{
    s_confirmedRenderQueue.Clear();
    s_awaitingTracker.Clear();
    exitOriginal(code);
}

bool InstallSubmissionHook(ID3D12Device* device, ID3D12CommandQueue* q)
{
    if (submissionHookReady)
        return true;
    executeOriginal = reinterpret_cast<ExecuteFn>((*reinterpret_cast<void***>(q))[10]);
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
        return false;
    DlssNr::Submission::NoteRawExecuteCommandLists(executeOriginal);
    submissionHookReady = true;
    return true;
}
} // namespace

void UpdateConfirmedRenderQueue(ID3D12CommandQueue* q)
{
    SetConfirmedRenderQueueInternal(q);
}

bool EnsureSubmissionHook(ID3D12CommandQueue* q)
{
    if (!q)
        return false;
    std::lock_guard lock(initMutex);
    if (submissionHookReady)
        return true;
    ID3D12Device* device = nullptr;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&device))))
        return false;
    const bool ready = InstallSubmissionHook(device, q);
    device->Release();
    return ready;
}

bool EnsureRecordingSubmissionHook(ID3D12CommandQueue* q)
{
    if (!EnsureSubmissionHook(q))
        return false;
    DlssNr::Submission::Hooks::EnableExpansion();
    return true;
}

bool HasDanielRuntime() { return false; }
bool HasLmxxfRuntime() { return false; }
bool HasFiles() { return false; }
bool IsRunning() { return false; }

void SyncBackendWithConfig()
{
    DlssNr::Backend::InvalidateInstallProbe();
    if (DlssNr::Submission::Hooks::IsArmed() && DlssNr::Backend::ProxyWrapWanted())
        DlssNr::Submission::Hooks::SetProxyWrap(true);
}

bool Evaluate(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, ID3D12CommandQueue*, bool,
              SrOutputExtent, std::optional<D3D12_RESOURCE_STATES>)
{
    return false;
}

bool HasReplacement(NVSDK_NGX_Parameter*) { return false; }
void Restore(NVSDK_NGX_Parameter*) {}
void InvalidateHistory() {}
void PollReleases() {}
void OnNrDisabled() {}
void TraceContextRelease(unsigned int, bool) {}
bool GraphicsRestartNeeded(UINT) { return false; }

DlssNr::Backend::Kind TimingBackendKind() { return DlssNr::Backend::Kind::Mochizuki; }
uint64_t TimingRuntimeId() { return 0; }
NrTimingSnapshot Timing() { return {}; }
MochizukiNrInfo MochizukiTimingDetails() { return {}; }
MochizukiNrBuildProgress BuildProgress() { return {}; }

DlssNr::Backend::InlineAutoSpin::StatusSnapshot MochizukiInlineAutoSpinStatus()
{
    const auto* c = Config::Instance();
    DlssNr::Backend::InlineAutoSpin::RuntimeConfigInput autoCfg {};
    autoCfg.inlineEnabled = DlssNr::AsiTakeover::NrRuntimeEnabled() && c->NrInline.value_or_default();
    autoCfg.isMochizuki = true;
    autoCfg.gpuHandoff = c->NrGpuHandoff.value_or_default();
    autoCfg.autoSpinEnabled = c->NrInlineAutoSpin.value_or_default();
    autoCfg.adaptiveWaitBudget = autoCfg.gpuHandoff && c->NrAdaptiveWaitBudget.value_or_default();
    if (c->NrSpinPollDelay.has_value()) autoCfg.fixedPollDelay = c->NrSpinPollDelay.value();
    if (c->NrSpinSlices.has_value()) autoCfg.fixedSlices = c->NrSpinSlices.value();
    DlssNr::Backend::InlineAutoSpin::Controller temp {};
    temp.UpdateConfigAndWorkload(autoCfg, {});
    return temp.GetStatus();
}

void RequestMochizukiInlineAutoSpinReeval() {}
std::string EffectsStatus() { return {}; }
const char* RuntimeName() { return nullptr; }
std::string Status()
{
    return "NR is managed by dlssnr-amd.asi";
}
} // namespace DlssNr::AmdBridge
