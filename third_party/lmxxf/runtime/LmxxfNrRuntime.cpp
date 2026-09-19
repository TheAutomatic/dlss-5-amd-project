#include "LmxxfNrApi.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <exception>
#include <new>
#include <string>

namespace
{
thread_local char g_lastError[256] = {};

void SetError(const char *text)
{
    if (!text)
        text = "";
    std::strncpy(g_lastError, text, sizeof(g_lastError) - 1);
    g_lastError[sizeof(g_lastError) - 1] = 0;
}

int32_t Fail(int32_t status, const char *text)
{
    SetError(text);
    return status;
}

struct Session
{
    void *device = nullptr;
    void *queue = nullptr;
};

template <class Fn>
int32_t Guard(Fn &&fn)
{
    try
    {
        return fn();
    }
    catch (const std::exception &ex)
    {
        return Fail(LMXXF_NR_FAILED, ex.what());
    }
    catch (...)
    {
        return Fail(LMXXF_NR_FAILED, "unhandled exception");
    }
}

int32_t QueryCapabilities(LmxxfNrCapabilities *out)
{
    return Guard([&] {
        if (!out)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "QueryCapabilities: null out");
        if (out->struct_size != sizeof(LmxxfNrCapabilities))
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "QueryCapabilities: struct_size mismatch");
        out->abi_version = LMXXF_NR_ABI_VERSION;
        out->max_input_width = 1920;
        out->max_input_height = 1080;
        out->history_supported = 0;
        out->overlap_supported = 0;
        out->graph_supported = 0;
        out->hip_ready = 0;
        out->gfx1201_target = 1;
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

int32_t Create(const LmxxfNrCreateInfo *info, void **context)
{
    return Guard([&] {
        if (!info || !context)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: null info or context");
        *context = nullptr;
        if (info->struct_size != sizeof(LmxxfNrCreateInfo))
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: struct_size mismatch");
        if (info->flags != 0)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: flags must be 0");
        if (!info->device || !info->queue)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "Create: device and queue required");
        auto *session = new Session;
        session->device = info->device;
        session->queue = info->queue;
        *context = session;
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

int32_t Destroy(void *context)
{
    return Guard([&] {
        delete static_cast<Session *>(context);
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

int32_t PrepareSession(void *context)
{
    return Guard([&] {
        if (!context)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareSession: null context");
        return Fail(LMXXF_NR_NOT_IMPLEMENTED, "HIP session is not wired");
    });
}

int32_t PrepareFrame(void *context, const LmxxfNrFrameInfo *info, LmxxfNrJob *job)
{
    return Guard([&] {
        if (!context || !info || !job)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: null argument");
        if (info->struct_size != sizeof(LmxxfNrFrameInfo) || job->struct_size != sizeof(LmxxfNrJob))
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "PrepareFrame: struct_size mismatch");
        job->handle = nullptr;
        job->private_output = nullptr;
        return Fail(LMXXF_NR_NOT_IMPLEMENTED, "PrepareFrame is not wired");
    });
}

int32_t NotWired(void *context, const char *name)
{
    if (!context)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "null context");
    return Fail(LMXXF_NR_NOT_IMPLEMENTED, name);
}

int32_t RecordInputs(void *context, void *, void *)
{
    return Guard([&] { return NotWired(context, "RecordInputs is not wired"); });
}
int32_t EnqueueHip(void *context, void *)
{
    return Guard([&] { return NotWired(context, "EnqueueHip is not wired"); });
}
int32_t RecordOutputs(void *context, void *, void *)
{
    return Guard([&] { return NotWired(context, "RecordOutputs is not wired"); });
}
int32_t ExecuteAfterProducer(void *context, void *)
{
    return Guard([&] { return NotWired(context, "ExecuteAfterProducer is not wired"); });
}
int32_t CancelUnsubmitted(void *context, void *)
{
    return Guard([&] { return NotWired(context, "CancelUnsubmitted is not wired"); });
}
int32_t Poll(void *context, void *, uint32_t *state)
{
    return Guard([&] {
        if (state)
            *state = LMXXF_NR_JOB_NONE;
        return NotWired(context, "Poll is not wired");
    });
}
int32_t Retire(void *context, void *)
{
    return Guard([&] { return NotWired(context, "Retire is not wired"); });
}
int32_t ResetHistory(void *context)
{
    return Guard([&] { return NotWired(context, "ResetHistory is not wired"); });
}
int32_t Drain(void *context)
{
    return Guard([&] { return NotWired(context, "Drain is not wired"); });
}

int32_t GetStatus(void *context, char *buf, uint32_t buf_chars)
{
    return Guard([&] {
        if (!buf || buf_chars == 0)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetStatus: empty buffer");
        const char *text = context ? "lmxxf runtime stub (HIP not wired)" : "no session";
        std::strncpy(buf, text, buf_chars - 1);
        buf[buf_chars - 1] = 0;
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

int32_t GetLastError(char *buf, uint32_t buf_chars)
{
    return Guard([&] {
        if (!buf || buf_chars == 0)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetLastError: empty buffer");
        std::strncpy(buf, g_lastError, buf_chars - 1);
        buf[buf_chars - 1] = 0;
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}
} // namespace

extern "C" int32_t LmxxfNrGetApi(uint32_t abi_version, LmxxfNrApi *out)
{
    return Guard([&] {
        if (!out)
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetApi: null out");
        if (out->struct_size != sizeof(LmxxfNrApi))
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "GetApi: struct_size mismatch");
        if (abi_version != LMXXF_NR_ABI_VERSION)
            return Fail(LMXXF_NR_UNSUPPORTED_ABI, "GetApi: unsupported abi_version");
        std::memset(out, 0, sizeof(*out));
        out->struct_size = sizeof(LmxxfNrApi);
        out->abi_version = LMXXF_NR_ABI_VERSION;
        out->QueryCapabilities = QueryCapabilities;
        out->Create = Create;
        out->Destroy = Destroy;
        out->PrepareSession = PrepareSession;
        out->PrepareFrame = PrepareFrame;
        out->RecordInputs = RecordInputs;
        out->EnqueueHip = EnqueueHip;
        out->RecordOutputs = RecordOutputs;
        out->ExecuteAfterProducer = ExecuteAfterProducer;
        out->CancelUnsubmitted = CancelUnsubmitted;
        out->Poll = Poll;
        out->Retire = Retire;
        out->ResetHistory = ResetHistory;
        out->Drain = Drain;
        out->GetStatus = GetStatus;
        out->GetLastError = GetLastError;
        SetError("");
        return static_cast<int32_t>(LMXXF_NR_OK);
    });
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        return TRUE;
    return TRUE;
}
