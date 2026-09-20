#include "pch.h"
#include "LmxxfBackend.h"
#include "../../../../third_party/lmxxf/include/LmxxfNrApi.h"
#include <cstdlib>
#include <fstream>
#include <string>

namespace DlssNr::Backend
{
struct LmxxfBackend::Api
{
    LmxxfNrApi table {};
};

namespace
{
std::wstring WidenPath(const std::filesystem::path &p) { return p.wstring(); }

std::filesystem::path ResolveModulesDir(const std::filesystem::path &directory)
{
    wchar_t env[MAX_PATH] {};
    if (GetEnvironmentVariableW(L"LMXXF_MODULES_DIR", env, MAX_PATH) && env[0])
        return env;
    const auto nextToDll = directory / L"lmxxf-modules";
    if (std::filesystem::exists(nextToDll))
        return nextToDll;
    // Dev layout: repo exports/lmxxf-modules-68dc099 relative to OptiScaler.dll parent is uncommon;
    // prefer env. Fall back to directory itself so Create can still run and fail loudly.
    return directory;
}
} // namespace

void LmxxfBackend::SetStatus(const char *s)
{
    if (!s)
        return;
    if (status == s)
        return;
    status = s;
    // Surface to OptiScaler.log once per distinct status (Record path was silent before).
    LOG_INFO("lmxxf status: {}", status);
}

LmxxfBackend::LmxxfBackend(ID3D12Device *dev, ID3D12CommandQueue *q, const std::filesystem::path &dir)
    : device(dev), queue(q), directory(dir)
{
    if (device)
        device->AddRef();
    if (queue)
        queue->AddRef();
    api = new Api();
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
    if (runtimeDll && api && api->table.EnqueueHip)
        return true;
    const auto dllPath = directory / L"LmxxfNrRuntime.dll";
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
    if (getApi(LMXXF_NR_ABI_VERSION, &api->table) != LMXXF_NR_OK)
    {
        SetStatus("lmxxf: GetApi failed");
        return false;
    }
    return true;
}

bool LmxxfBackend::EnsureSession()
{
    if (sessionReady && session)
        return true;
    if (!EnsureRuntime() || !device || !queue)
        return false;

    // Upstream 0.21+: auto picks 720/900/1080 from Color size. Unset defaults to 1080 and blacks 720p Color.
    if (!std::getenv("DLSS5_NETWORK_HEIGHT"))
    {
        _putenv("DLSS5_NETWORK_HEIGHT=auto");
        SetEnvironmentVariableA("DLSS5_NETWORK_HEIGHT", "auto");
        LOG_INFO("lmxxf: DLSS5_NETWORK_HEIGHT defaulted to auto");
    }
    // Runtime FindWeightsDir reads LMXXF_WEIGHTS_DIR; some launchers omit User env.
    // Promote User/Machine value into this process, or accept a sibling hint file.
    {
        wchar_t have[MAX_PATH] {};
        if (!GetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", have, MAX_PATH) || !have[0])
        {
            wchar_t fromUser[MAX_PATH] {};
            DWORD n = GetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", fromUser, MAX_PATH);
            (void)n;
            const auto hint = directory / L"lmxxf-weights-dir.txt";
            if (std::filesystem::exists(hint))
            {
                std::wifstream in(hint);
                std::wstring line;
                if (in && std::getline(in, line) && !line.empty())
                {
                    while (!line.empty() && (line.back() == L'\r' || line.back() == L' '))
                        line.pop_back();
                    SetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", line.c_str());
                    LOG_INFO("lmxxf: LMXXF_WEIGHTS_DIR from hint file: {}", std::filesystem::path(line).string());
                }
            }
        }
        wchar_t now[MAX_PATH] {};
        if (GetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", now, MAX_PATH) && now[0])
            LOG_INFO("lmxxf: LMXXF_WEIGHTS_DIR={}", std::filesystem::path(now).string());
        else
            LOG_WARN("lmxxf: LMXXF_WEIGHTS_DIR unset (PrepareSession may fail without tiled weights)");
    }

    const auto modules = ResolveModulesDir(directory);
    const std::wstring modulesW = WidenPath(modules);
    LOG_INFO("lmxxf: assets/modules dir={}", modules.string());
    LmxxfNrCreateInfo info {};
    info.struct_size = sizeof(info);
    info.device = device;
    info.queue = queue;
    info.assets_directory = modulesW.c_str();
    info.flags = 0;
    void *ctx = nullptr;
    const int32_t createRc = api->table.Create(&info, &ctx);
    if (createRc != LMXXF_NR_OK || !ctx)
    {
        char err[256] {};
        if (api->table.GetLastError)
            api->table.GetLastError(err, sizeof err);
        LOG_ERROR("lmxxf: Create rc={} err={}", createRc, err);
        SetStatus("lmxxf: Create failed");
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
        SetStatus("lmxxf: PrepareSession failed");
        return false;
    }
    session = ctx;
    sessionReady = true;
    SetStatus("lmxxf: session ready");
    return true;
}


bool LmxxfBackend::EnsurePrivateList()
{
    if (!device || !queue)
        return false;
    if (!privFence)
    {
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&privFence))))
            return false;
        privFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!privFenceEvent)
            return false;
    }
    if (privFenceValue != 0 && privFence->GetCompletedValue() < privFenceValue)
    {
        if (FAILED(privFence->SetEventOnCompletion(privFenceValue, privFenceEvent)))
            return false;
        if (WaitForSingleObject(privFenceEvent, 2000) != WAIT_OBJECT_0)
            return false;
    }
    if (!privAlloc)
    {
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&privAlloc))))
            return false;
    }
    else
    {
        if (FAILED(privAlloc->Reset()))
            return false;
    }
    if (!privCmd)
    {
        if (FAILED(DlssNr::Submission::Hooks::CreateProxiedCommandList(
                device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, privAlloc, nullptr, IID_PPV_ARGS(&privCmd))))
            return false;
    }
    else
    {
        if (FAILED(privCmd->Reset(privAlloc, nullptr)))
            return false;
    }
    return true;
}

void LmxxfBackend::ReleasePrivateList()
{
    if (privCmd)
    {
        privCmd->Release();
        privCmd = nullptr;
    }
    if (privAlloc)
    {
        privAlloc->Release();
        privAlloc = nullptr;
    }
    if (privFenceEvent)
    {
        CloseHandle(privFenceEvent);
        privFenceEvent = nullptr;
    }
    if (privFence)
    {
        privFence->Release();
        privFence = nullptr;
    }
    privFenceValue = 0;
}

ID3D12Resource *LmxxfBackend::FinishRecord(ID3D12GraphicsCommandList *recordCmd, void *jobHandle,
                                           void *privateOutput, bool executeNow)
{
    if (api->table.RecordInputs(session, jobHandle, recordCmd) != LMXXF_NR_OK)
    {
        api->table.CancelUnsubmitted(session, jobHandle);
        SetStatus("lmxxf: RecordInputs failed");
        return nullptr;
    }
    const HRESULT splitHr = LmxxfCut::TrySplitAtEvaluate(recordCmd);
    if (FAILED(splitHr) || splitHr == S_FALSE)
    {
        api->table.CancelUnsubmitted(session, jobHandle);
        SetStatus(FAILED(splitHr) ? "lmxxf: Split failed" : "lmxxf: Split returned S_FALSE");
        return nullptr;
    }
    if (api->table.RecordOutputs(session, jobHandle, recordCmd) != LMXXF_NR_OK)
    {
        api->table.CancelUnsubmitted(session, jobHandle);
        SetStatus("lmxxf: RecordOutputs failed");
        return nullptr;
    }
    LmxxfCut::SetPendingEnqueue(session, jobHandle, api->table.EnqueueHip);
    LmxxfCut::ArmBetweenSlot();
    if (executeNow)
    {
        DlssNr::Submission::ILogicalCommandList *logical = nullptr;
        if (FAILED(recordCmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                             reinterpret_cast<void **>(&logical))) ||
            !logical)
        {
            api->table.CancelUnsubmitted(session, jobHandle);
            LmxxfCut::ClearPendingEnqueue();
            SetStatus("lmxxf: private list lost ILogicalCommandList");
            return nullptr;
        }
        const HRESULT ex = logical->ExecuteOnWithBetween(queue, &LmxxfCut::BetweenThunk, nullptr);
        logical->Release();
        if (FAILED(ex))
        {
            api->table.CancelUnsubmitted(session, jobHandle);
            LmxxfCut::ClearPendingEnqueue();
            SetStatus("lmxxf: private ExecuteOnWithBetween failed");
            return nullptr;
        }
        const UINT64 signal = ++privFenceValue;
        if (FAILED(queue->Signal(privFence, signal)))
            LOG_WARN("lmxxf: private Signal failed after Execute");
        if (api->table.Retire)
            api->table.Retire(session, jobHandle);
        LmxxfCut::ClearPendingEnqueue();
        pendingJob = nullptr;
        const int32_t hipRc = LmxxfCut::Pending().lastEnqueueRc.load(std::memory_order_relaxed);
        SetStatus(hipRc == 0 ? "lmxxf: Record ok (private CL EnqueueHip)"
                             : "lmxxf: Record ok (private CL; EnqueueHip rc!=0)");
        LOG_INFO("lmxxf: private CL betweenHits={} EnqueueHip rc={}",
                 LmxxfCut::Pending().betweenHits.load(std::memory_order_relaxed), hipRc);
        return reinterpret_cast<ID3D12Resource *>(privateOutput);
    }
    pendingJob = jobHandle;
    SetStatus("lmxxf: Record ok (pending EnqueueHip)");
    return reinterpret_cast<ID3D12Resource *>(privateOutput);
}


ID3D12Resource *LmxxfBackend::Record(ID3D12GraphicsCommandList *cmd, const AmdPreSr::Frame &frame,
                                     const AmdPreSr::Settings & /* strength/menu unused: ABI v1 colour-only */)
{
    LmxxfCut::ClearPendingEnqueue();
    pendingJob = nullptr;
    if (!cmd || !frame.colour)
    {
        SetStatus("lmxxf: Record missing cmd/colour");
        return nullptr;
    }
    if (!EnsureSession())
        return nullptr;

    D3D12_RESOURCE_DESC desc = frame.colour->GetDesc();
    LmxxfNrFrameInfo fi {};
    fi.struct_size = sizeof(fi);
    fi.frame_id = ++frameId;
    fi.command_list = cmd;
    fi.color_width = frame.width ? frame.width : static_cast<uint32_t>(desc.Width);
    fi.color_height = frame.height ? frame.height : static_cast<uint32_t>(desc.Height);
    fi.color = frame.colour;
    fi.color_state = static_cast<uint32_t>(frame.colourState);
    fi.flags = 0;

    LmxxfNrJob job {};
    job.struct_size = sizeof(job);
    const int32_t frameRc = api->table.PrepareFrame(session, &fi, &job);
    if (frameRc != LMXXF_NR_OK || !job.handle || !job.private_output)
    {
        char err[256] {};
        if (api->table.GetLastError)
            api->table.GetLastError(err, sizeof err);
        static unsigned prepareFrameFailLogs = 0;
        if (prepareFrameFailLogs < 3 || (prepareFrameFailLogs % 120) == 0)
            LOG_ERROR("lmxxf: PrepareFrame rc={} handle={} out={} err={} {}x{} (fail#{})", frameRc,
                      job.handle != nullptr, job.private_output != nullptr, err, fi.color_width,
                      fi.color_height, prepareFrameFailLogs + 1);
        ++prepareFrameFailLogs;
        SetStatus("lmxxf: PrepareFrame failed");
        return nullptr;
    }
    {
        static bool loggedGeo = false;
        if (!loggedGeo && api->table.GetStatus)
        {
            char st[256] {};
            api->table.GetStatus(session, st, sizeof st);
            LOG_INFO("lmxxf: after PrepareFrame HIP/net geometry status={}", st);
            loggedGeo = true;
        }
    }

    DlssNr::Submission::ILogicalCommandList *logical = nullptr;
    const bool isProxy =
        SUCCEEDED(cmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                      reinterpret_cast<void **>(&logical))) &&
        logical;
    if (logical)
        logical->Release();

    if (!isProxy)
    {
        // yysls/Streamline Evaluate uses CreateCommandList; CL1-only wrap never sees it.
        // Open Create wrap DEVICE_REMOVEs — private proxied list + ExecuteOnWithBetween now.
        if (!EnsurePrivateList())
        {
            api->table.CancelUnsubmitted(session, job.handle);
            SetStatus("lmxxf: private CL setup failed");
            return nullptr;
        }
        return FinishRecord(privCmd, job.handle, job.private_output, true);
    }

    return FinishRecord(cmd, job.handle, job.private_output, false);
}


// Daniel uses PendingListIndex to isolate a private neural list from a multi-list batch.
// lmxxf HIP sits in ExecuteExpanded's between-slot on the game proxy itself, so isolation
// is unnecessary: AmdBridge::ExecuteBatch always calls ExecuteExpanded when ExpandEnabled().
int LmxxfBackend::PendingListIndex(UINT, ID3D12CommandList *const *) const { return -1; }

void LmxxfBackend::Submitting(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *) {}

void LmxxfBackend::TraceBoundary(const std::string &) {}

void LmxxfBackend::Submitted(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *)
{
    if (session && pendingJob && api && api->table.Retire)
    {
        api->table.Retire(session, pendingJob);
        pendingJob = nullptr;
    }
    LmxxfCut::ClearPendingEnqueue();
}

bool LmxxfBackend::Shutdown()
{
    LmxxfCut::DisarmBetweenSlot();
    ReleasePrivateList();
    pendingJob = nullptr;
    if (session && api && api->table.Destroy)
    {
        api->table.Destroy(session);
        session = nullptr;
    }
    sessionReady = false;
    if (runtimeDll)
    {
        FreeLibrary(reinterpret_cast<HMODULE>(runtimeDll));
        runtimeDll = nullptr;
    }
    if (api)
        api->table = {};
    SetStatus("lmxxf: shutdown");
    return true;
}

void LmxxfBackend::InvalidateHistory()
{
    if (session && api && api->table.ResetHistory)
        api->table.ResetHistory(session);
}

std::string LmxxfBackend::Status() const { return status; }

bool LmxxfBackend::GraphicsRestartNeeded(UINT) const { return false; }
} // namespace DlssNr::Backend
