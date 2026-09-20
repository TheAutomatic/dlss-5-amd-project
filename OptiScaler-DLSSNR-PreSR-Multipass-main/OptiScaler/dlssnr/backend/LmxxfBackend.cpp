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
        static unsigned prepareFrameFailLogs = 0;
        if (prepareFrameFailLogs < 3 || (prepareFrameFailLogs % 120) == 0)
            LOG_ERROR("lmxxf: PrepareFrame rc={} handle={} out={} err={} {}x{} (fail#{})", frameRc,
                      job.handle != nullptr, job.private_output != nullptr, err, fi.color_width,
                      fi.color_height, prepareFrameFailLogs + 1);
        ++prepareFrameFailLogs;
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
