#include "pch.h"
#include "LmxxfBackend.h"
#include "../../../../third_party/lmxxf/include/LmxxfNrApi.h"
#include <cstdlib>

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
    const auto modules = ResolveModulesDir(directory);
    const std::wstring modulesW = WidenPath(modules);
    LmxxfNrCreateInfo info {};
    info.struct_size = sizeof(info);
    info.device = device;
    info.queue = queue;
    info.assets_directory = modulesW.c_str();
    info.flags = 0;
    void *ctx = nullptr;
    if (api->table.Create(&info, &ctx) != LMXXF_NR_OK || !ctx)
    {
        SetStatus("lmxxf: Create failed");
        return false;
    }
    if (api->table.PrepareSession(ctx) != LMXXF_NR_OK)
    {
        api->table.Destroy(ctx);
        SetStatus("lmxxf: PrepareSession failed");
        return false;
    }
    session = ctx;
    sessionReady = true;
    SetStatus("lmxxf: session ready");
    return true;
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
    if (api->table.PrepareFrame(session, &fi, &job) != LMXXF_NR_OK || !job.handle || !job.private_output)
    {
        SetStatus("lmxxf: PrepareFrame failed");
        return nullptr;
    }

    // Fail-closed: no ILogicalCommandList proxy → cannot HIP-sandwich after producer submit.
    // Do not RecordInputs / EnqueueHip / return private_output (would violate submit contract
    // and hand NGX a not-ready replacement). Ordinary SR keeps the original colour.
    {
        DlssNr::Submission::ILogicalCommandList *logical = nullptr;
        const bool isProxy =
            SUCCEEDED(cmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                          reinterpret_cast<void **>(&logical))) &&
            logical;
        if (logical)
            logical->Release();
        if (!isProxy)
        {
            api->table.CancelUnsubmitted(session, job.handle);
            SetStatus("lmxxf: no command-list proxy; skip NR (ordinary SR)");
            return nullptr;
        }
    }

    // Producer side: RecordInputs while list is still unsplit.
    if (api->table.RecordInputs(session, job.handle, cmd) != LMXXF_NR_OK)
    {
        api->table.CancelUnsubmitted(session, job.handle);
        SetStatus("lmxxf: RecordInputs failed");
        return nullptr;
    }

    // Cut: close producer / open continuation, then RecordOutputs on continuation.
    const HRESULT splitHr = LmxxfCut::TrySplitAtEvaluate(cmd);
    if (FAILED(splitHr) || splitHr == S_FALSE)
    {
        api->table.CancelUnsubmitted(session, job.handle);
        SetStatus(FAILED(splitHr) ? "lmxxf: Split failed" : "lmxxf: Split returned S_FALSE");
        return nullptr;
    }
    if (api->table.RecordOutputs(session, job.handle, cmd) != LMXXF_NR_OK)
    {
        api->table.CancelUnsubmitted(session, job.handle);
        SetStatus("lmxxf: RecordOutputs failed");
        return nullptr;
    }

    LmxxfCut::SetPendingEnqueue(session, job.handle, api->table.EnqueueHip);
    LmxxfCut::ArmBetweenSlot();
    pendingJob = job.handle;
    SetStatus("lmxxf: Record ok (pending EnqueueHip)");
    return reinterpret_cast<ID3D12Resource *>(job.private_output);
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
