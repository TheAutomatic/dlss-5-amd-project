#include "pch.h"
#include "LmxxfBackend.h"
#include <cstring>
#include "../submission/SubmissionTls.h"
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



void LmxxfBackend::ReleaseColorRing()
{
    for (auto &r : colorRing)
    {
        if (r)
        {
            r->Release();
            r = nullptr;
        }
    }
    colorRingReady[0] = colorRingReady[1] = false;
    colorRingWrite = 0;
    colorRingW = colorRingH = 0;
    colorRingFmt = DXGI_FORMAT_UNKNOWN;
}

bool LmxxfBackend::EnsureColorRing(ID3D12Resource *color)
{
    if (!device || !color)
        return false;
    const D3D12_RESOURCE_DESC d = color->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        return false;
    const UINT w = static_cast<UINT>(d.Width);
    const UINT h = d.Height;
    if (colorRing[0] && colorRing[1] && colorRingW == w && colorRingH == h && colorRingFmt == d.Format)
        return true;
    ReleaseColorRing();
    D3D12_HEAP_PROPERTIES hp {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = d;
    td.Alignment = 0;
    td.Flags = D3D12_RESOURCE_FLAG_NONE; // copy dest / SRV for encode
    for (int i = 0; i < 2; ++i)
    {
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&colorRing[i]))) ||
            !colorRing[i])
        {
            ReleaseColorRing();
            return false;
        }
    }
    colorRingW = w;
    colorRingH = h;
    colorRingFmt = d.Format;
    return true;
}

void LmxxfBackend::ScheduleColorCapture(ID3D12GraphicsCommandList *gameCmd, ID3D12Resource *color,
                                        D3D12_RESOURCE_STATES colorState, UINT slot)
{
    if (!gameCmd || !color || slot > 1 || !colorRing[slot])
        return;
    ID3D12Resource *dst = colorRing[slot];
    D3D12_RESOURCE_BARRIER b[2] {};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition = {color, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, colorState,
                       D3D12_RESOURCE_STATE_COPY_SOURCE};
    // Ring slot may be COMMON (first use) or NON_PIXEL_SHADER_RESOURCE (after prior capture).
    const D3D12_RESOURCE_STATES dstBefore =
        colorRingReady[slot] ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
    b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[1].Transition = {dst, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, dstBefore,
                       D3D12_RESOURCE_STATE_COPY_DEST};
    gameCmd->ResourceBarrier(2, b);
    gameCmd->CopyResource(dst, color);
    // Restore game Color to the state Evaluate advertised (caller's contract).
    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter = colorState;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    gameCmd->ResourceBarrier(2, b);
    colorRingReady[slot] = true;
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
        LOG_INFO("lmxxf: private CL betweenHits={} enqueueCalls={} skipped={} EnqueueHip rc={}",
                 LmxxfCut::Pending().betweenHits.load(std::memory_order_relaxed),
                 LmxxfCut::Pending().enqueueCalls.load(std::memory_order_relaxed),
                 LmxxfCut::Pending().skippedHits.load(std::memory_order_relaxed), hipRc);
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
        static unsigned prepareFrameRebuilds = 0;
        if (prepareFrameFailLogs < 3 || (prepareFrameFailLogs % 30) == 0)
            LOG_ERROR("lmxxf: PrepareFrame rc={} handle={} out={} err={} {}x{} (fail#{})", frameRc,
                      job.handle != nullptr, job.private_output != nullptr, err, fi.color_width,
                      fi.color_height, prepareFrameFailLogs + 1);
        ++prepareFrameFailLogs;
        // Menu/resize: runtime drains/rebuilds codec on rebind/geometry; if still failing,
        // drop host session so the next Record EnsureSession starts clean.
        const bool rebindish = frameRc == LMXXF_NR_UNAVAILABLE || (err[0] && (std::strstr(err, "rebind") || std::strstr(err, "geometry")));
        if (rebindish && (prepareFrameFailLogs <= 2 || (prepareFrameFailLogs % 4) == 0))
        {
            LOG_WARN("lmxxf: PrepareFrame fail -> host session rebuild #{}", ++prepareFrameRebuilds);
            ReleaseColorRing();
            if (session && api && api->table.Destroy)
                api->table.Destroy(session);
            session = nullptr;
            sessionReady = false;
            SetStatus("lmxxf: session rebuild after PrepareFrame fail");
        }
        else
        {
            SetStatus("lmxxf: PrepareFrame failed");
        }
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
        // Color may still be unsubmitted on `cmd`: capture THIS frame onto the game list,
        // NR from the PREVIOUS capture (1-frame lag, upstream overlap style).
        if (!EnsureColorRing(frame.colour))
        {
            api->table.CancelUnsubmitted(session, job.handle);
            SetStatus("lmxxf: Color ring alloc failed");
            return nullptr;
        }
        const UINT captureSlot = colorRingWrite;
        const UINT nrSlot = colorRingWrite ^ 1u;
        ScheduleColorCapture(cmd, frame.colour,
                             static_cast<D3D12_RESOURCE_STATES>(frame.colourState), captureSlot);
        colorRingWrite ^= 1u;
        if (!colorRingReady[nrSlot])
        {
            // First frame(s): capture scheduled, nothing safe to NR yet — keep original Color.
            api->table.CancelUnsubmitted(session, job.handle);
            SetStatus("lmxxf: private CL priming Color capture");
            return nullptr;
        }
        if (!EnsurePrivateList())
        {
            api->table.CancelUnsubmitted(session, job.handle);
            SetStatus("lmxxf: private CL setup failed");
            return nullptr;
        }
        // Re-Prepare against the previous capture so encode/decode see a completed Color.
        AmdPreSr::Frame nrFrame = frame;
        nrFrame.colour = colorRing[nrSlot];
        nrFrame.colourState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        nrFrame.width = colorRingW;
        nrFrame.height = colorRingH;
        D3D12_RESOURCE_DESC nrDesc = nrFrame.colour->GetDesc();
        LmxxfNrFrameInfo fi2 {};
        fi2.struct_size = sizeof(fi2);
        fi2.frame_id = fi.frame_id;
        fi2.command_list = privCmd;
        fi2.color_width = nrFrame.width ? nrFrame.width : static_cast<uint32_t>(nrDesc.Width);
        fi2.color_height = nrFrame.height ? nrFrame.height : static_cast<uint32_t>(nrDesc.Height);
        fi2.color = nrFrame.colour;
        fi2.color_state = static_cast<uint32_t>(nrFrame.colourState);
        fi2.flags = 0;
        LmxxfNrJob job2 {};
        job2.struct_size = sizeof(job2);
        const int32_t frameRc2 = api->table.PrepareFrame(session, &fi2, &job2);
        if (frameRc2 != LMXXF_NR_OK || !job2.handle || !job2.private_output)
        {
            char err[256] {};
            if (api->table.GetLastError)
                api->table.GetLastError(err, sizeof err);
            LOG_ERROR("lmxxf: PrepareFrame(staging) rc={} err={}", frameRc2, err);
            api->table.CancelUnsubmitted(session, job.handle);
            SetStatus("lmxxf: PrepareFrame staging failed");
            return nullptr;
        }
        api->table.CancelUnsubmitted(session, job.handle); // drop the live-Color job; use staging job
        return FinishRecord(privCmd, job2.handle, job2.private_output, true);
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
    // LogicalList producer/continuation submit re-enters this hook; clearing here
    // would drop HIP before BetweenThunk. BetweenThunk consumes Pending itself.
    if (!DlssNr::Submission::InsideLogicalExecute())
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
