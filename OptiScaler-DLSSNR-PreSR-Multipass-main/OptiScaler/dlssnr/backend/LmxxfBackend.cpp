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
    diagnostic = LmxxfProbe::ParseMode(Config::Instance()->LmxxfDiagnostic.value_or_default());
    LOG_INFO("lmxxf diagnostic: mode={} (restart to change; off/original/copy-current/staging-current/staging-previous/proxy-original/split-original)",
             Config::Instance()->LmxxfDiagnostic.value_or_default());
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



ID3D12Resource *LmxxfBackend::FinishRecord(ID3D12GraphicsCommandList *recordCmd, void *jobHandle,
                                           void *privateOutput)
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
    // Crucially before EnsureSession: controls do not load the runtime, prepare HIP,
    // or submit HIP. split-original only cuts the game list for boundary validation.
    if (diagnostic != LmxxfProbe::Mode::Off)
        return RecordDiagnostic(cmd, frame);
    // Never substitute Color from an earlier Evaluate to work around an unsubmitted producer.
    DlssNr::Submission::ILogicalCommandList *logical = nullptr;
    if (FAILED(cmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                  reinterpret_cast<void **>(&logical))) || !logical)
    {
        SetStatus("lmxxf: same-frame boundary unavailable (original Color; NO NR)");
        return nullptr;
    }
    const bool ineligible = logical->IsSplitIneligible();
    logical->Release();
    if (ineligible)
    {
        SetStatus("lmxxf: same-frame split ineligible (original Color; NO NR)");
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

    ID3D12Resource *result = FinishRecord(cmd, job.handle, job.private_output);
    static uint64_t recordEvalCount = 0;
    const auto rEval = ++recordEvalCount;
    if (rEval <= 5 || (rEval % 120 == 0))
    {
        auto &p = LmxxfCut::Pending();
        LOG_INFO("lmxxf nr: eval={} output={} betweenHits={} enqueueCalls={} skippedHits={} lastEnqueueRc={:X} producerSubmitted={} continuationSubmitted={} submitFailures={}",
                 rEval, static_cast<void *>(result),
                 p.betweenHits.load(std::memory_order_relaxed),
                 p.enqueueCalls.load(std::memory_order_relaxed),
                 p.skippedHits.load(std::memory_order_relaxed),
                 static_cast<unsigned>(p.lastEnqueueRc.load(std::memory_order_relaxed)),
                 DlssNr::Submission::g_splitSubmissions.load(std::memory_order_relaxed),
                 DlssNr::Submission::g_continuationSubmissions.load(std::memory_order_relaxed),
                 DlssNr::Submission::g_submissionFailures.load(std::memory_order_relaxed));
    }
    return result;
}


ID3D12Resource *LmxxfBackend::RecordDiagnostic(ID3D12GraphicsCommandList *cmd, const AmdPreSr::Frame &frame)
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

    if (diagnostic == LmxxfProbe::Mode::CodecPassthrough)
    {
        DlssNr::Submission::ILogicalCommandList *logical = nullptr;
        const bool isProxy = SUCCEEDED(cmd->QueryInterface(__uuidof(DlssNr::Submission::ILogicalCommandList),
                                                         reinterpret_cast<void **>(&logical))) && logical;
        if (!isProxy)
        {
            reason = "boundary_not_proxy";
            ++boundaryRejects;
            SetStatus("lmxxf diagnostic: codec-passthrough REJECTED (not proxy; original Color)");
        }
        else
        {
            ++boundaryProxyHits;
            if (logical->IsSplitIneligible())
            {
                reason = logical->SplitRejectionReason();
                ++boundaryRejects;
                char state[256] {};
                snprintf(state, sizeof state, "lmxxf diagnostic: codec-passthrough REJECTED (%s; original Color)", reason);
                SetStatus(state);
            }
            else if (!EnsureSession())
            {
                reason = "ensure_session_failed";
                ++boundaryRejects;
                SetStatus("lmxxf diagnostic: codec-passthrough REJECTED (session failed; original Color)");
            }
            else
            {
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
                    static uint64_t diagPrepareFails = 0;
                    if (++diagPrepareFails <= 5 || (diagPrepareFails % 300 == 0))
                    {
                        LOG_ERROR("lmxxf diagnostic: codec-passthrough PrepareFrame rc={} handle={} out={} err='{}' {}x{} (fail#{})",
                                  frameRc, job.handle != nullptr, job.private_output != nullptr, err,
                                  fi.color_width, fi.color_height, diagPrepareFails);
                    }
                    reason = "prepare_frame_failed";
                    ++boundaryRejects;
                    SetStatus((std::string("lmxxf diagnostic: codec-passthrough PrepareFrame failed: ") + err).c_str());
                }
                else if (api->table.RecordInputs(session, job.handle, cmd) != LMXXF_NR_OK)
                {
                    char err[256] {};
                    if (api->table.GetLastError)
                        api->table.GetLastError(err, sizeof err);
                    LOG_ERROR("lmxxf diagnostic: codec-passthrough RecordInputs failed: {}", err);
                    reason = "record_inputs_failed";
                    ++boundaryRejects;
                    SetStatus("lmxxf diagnostic: codec-passthrough RecordInputs failed");
                }
                else
                {
                    const HRESULT cutHr = logical->SplitSegments();
                    if (cutHr != S_OK)
                    {
                        char err[256] {};
                        if (api->table.GetLastError)
                            api->table.GetLastError(err, sizeof err);
                        LOG_ERROR("lmxxf diagnostic: codec-passthrough Split failed: hr={:X} err='{}'", static_cast<unsigned>(cutHr), err);
                        reason = "boundary_split_failed";
                        ++boundaryRejects;
                        SetStatus("lmxxf diagnostic: codec-passthrough Split failed");
                    }
                    else
                    {
                        ++boundaryCuts;
                        if (api->table.RecordOutputs(session, job.handle, cmd) != LMXXF_NR_OK)
                        {
                            char err[256] {};
                            if (api->table.GetLastError)
                                api->table.GetLastError(err, sizeof err);
                            LOG_ERROR("lmxxf diagnostic: codec-passthrough RecordOutputs failed: {}", err);
                            reason = "record_outputs_failed";
                            ++boundaryRejects;
                            SetStatus("lmxxf diagnostic: codec-passthrough RecordOutputs failed");
                        }
                        else
                        {
                            output = reinterpret_cast<ID3D12Resource *>(job.private_output);
                            reason = "codec_passthrough_recorded";
                            SetStatus("lmxxf diagnostic: codec-passthrough (NO HIP; encode->decode passthrough)");
                        }
                    }
                }
            }
        }
        if (logical)
            logical->Release();
        if (sampled)
            LOG_INFO("lmxxf boundary: eval={} proxy={} reason={} proxyHits={} cutsRecorded={} rejected={} output={} unsplitSubmitted={} producerSubmitted={} continuationSubmitted={} submitFailures={}",
                     id, isProxy, reason, boundaryProxyHits, boundaryCuts, boundaryRejects, static_cast<void *>(output),
                     DlssNr::Submission::g_unsplitProxySubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_splitSubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_continuationSubmissions.load(std::memory_order_relaxed),
                     DlssNr::Submission::g_submissionFailures.load(std::memory_order_relaxed));
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
    stagingProbe.InvalidateEpoch();
}

std::string LmxxfBackend::Status() const { return status; }

bool LmxxfBackend::GraphicsRestartNeeded(UINT) const { return false; }
} // namespace DlssNr::Backend
