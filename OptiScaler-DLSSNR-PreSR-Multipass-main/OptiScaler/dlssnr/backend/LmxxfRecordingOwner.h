#pragma once
#include "LmxxfEvaluateCut.h"
#include "lmxxf_runtime/LmxxfNrApi.h"
#include <memory>
#include <vector>

namespace DlssNr::Backend::LmxxfRecording
{
struct SessionOwner
{
    LmxxfNrApi api {};
    void* context = nullptr;
    HMODULE module = nullptr;
    bool failed = false;
    ~SessionOwner()
    {
        if (context && api.Destroy(context) != LMXXF_NR_OK)
        {
            OutputDebugStringA("lmxxf: retaining session/module with outstanding recording jobs\n");
            return;
        }
        if (module) FreeLibrary(module);
    }
    static std::shared_ptr<SessionOwner> Create(const LmxxfNrApi& api, void* context)
    {
        std::shared_ptr<SessionOwner> owner;
        try { owner = std::make_shared<SessionOwner>(); }
        catch (...) { return {}; }
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(api.Destroy), &owner->module)) return {};
        owner->api = api; owner->context = context;
        return owner;
    }
};

struct Lease;
inline std::vector<std::shared_ptr<Lease>>& Registry()
{
    // Outstanding live recordings are retained at process exit; static teardown
    // must not destroy GPU resources while game lists still own executable code.
    static auto* leases = new std::vector<std::shared_ptr<Lease>>;
    return *leases;
}
inline bool CollectLocked();
inline void ScheduleCollectionLocked() noexcept;

struct Lease final : Submission::RecordingObserver
{
    std::shared_ptr<SessionOwner> owner;
    void* job;
    Submission::RecordingIdentity identity;
    bool ready = false, invalidated = false, begun = false;
    uint64_t traceId = 0;
    void (*trace)(uint64_t, const char*, const void*, int32_t) noexcept = nullptr;
    unsigned traceExecutions = 0;
    bool tracing = false;
    void Trace(const char* phase, const void* object, int32_t result = 0) const noexcept
    { if (tracing && trace && traceId) trace(traceId, phase, object, result); }
    Lease(std::shared_ptr<SessionOwner> session, void* token, Submission::RecordingIdentity id)
        : owner(std::move(session)), job(token), identity(id) {}
    HRESULT BeforeExecute(const Submission::RecordingExecution& e) noexcept override
    {
        if (invalidated || !(e.identity == identity)) return E_UNEXPECTED;
        tracing = traceId && traceExecutions < 2;
        if (tracing) ++traceExecutions;
        Trace("execute.begin", e.queue);
        const int32_t rc = owner->api.BeginRecordingExecution(owner->context, job, e.queue);
        Trace("execute.admitted", e.queue, rc);
        begun = rc == LMXXF_NR_OK;
        if (!begun) owner->failed = true;
        return begun ? S_OK : E_FAIL;
    }
    void Between(const Submission::RecordingExecution& e) noexcept override
    {
        if (!begun || !ready) return;
        auto& diagnostic = LmxxfCut::Pending();
        diagnostic.betweenHits.fetch_add(1, std::memory_order_relaxed);
        diagnostic.enqueueCalls.fetch_add(1, std::memory_order_relaxed);
        Trace("hip.begin", e.queue);
        const int32_t rc = owner->api.EnqueueHip(owner->context, job, e.queue);
        Trace("hip.end", e.queue, rc);
        std::array<char, 256> error {};
        owner->api.GetLastError(error.data(), static_cast<uint32_t>(error.size()));
        if (rc != LMXXF_NR_OK) owner->failed = true;
        std::lock_guard lock(diagnostic.mutex);
        diagnostic.lastEnqueueRc.store(rc, std::memory_order_relaxed);
        diagnostic.lastEnqueueError = error;
        diagnostic.lastEnqueueQueue = e.queue;
        diagnostic.lastQueue.store(e.queue, std::memory_order_relaxed);
    }
    void Executed(const Submission::RecordingExecution& e) noexcept override
    {
        if (!begun) return;
        const uint32_t flags = (e.producerSubmitted ? LMXXF_NR_SUBMITTED_PRODUCER : 0) |
                               (e.continuationSubmitted ? LMXXF_NR_SUBMITTED_CONSUMER : 0);
        Trace("retire.begin", e.queue, e.status);
        const int32_t rc = owner->api.EndRecordingExecution(owner->context, job, e.queue,
                                                          flags, e.fence, e.fenceValue, e.status);
        if (rc != LMXXF_NR_OK) owner->failed = true;
        Trace("retire.end", e.queue, rc);
        tracing = false;
        begun = false;
    }
    void Invalidated(Submission::RecordingIdentity id) noexcept override
    {
        if (invalidated || !(id == identity)) return;
        invalidated = true;
        if (owner->api.InvalidateRecording(owner->context, job) != LMXXF_NR_OK) owner->failed = true;
        if (CollectLocked()) ScheduleCollectionLocked();
    }
};

inline bool CollectLocked()
{
    bool pending = false;
    auto& leases = Registry();
    for (auto it = leases.begin(); it != leases.end();)
    {
        auto& lease = *it;
        if (!lease->invalidated) { ++it; continue; }
        const int32_t rc = lease->owner->api.CollectRecording(lease->owner->context, lease->job);
        if (rc == LMXXF_NR_OK || rc == LMXXF_NR_DEVICE_LOST)
        {
            lease->job = nullptr;
            it = leases.erase(it);
        }
        else { pending = true; ++it; }
    }
    return pending;
}

struct CollectionTimer
{
    PTP_TIMER timer = nullptr;
    HMODULE module = nullptr;
};
inline CollectionTimer& Timer() { static CollectionTimer timer; return timer; }
inline void Arm(PTP_TIMER timer) noexcept
{
    LARGE_INTEGER due {}; due.QuadPart = -1000000; // 100 ms, no GPU/CPU drain.
    FILETIME when {due.LowPart, static_cast<DWORD>(due.HighPart)};
    SetThreadpoolTimer(timer, &when, 0, 0);
}
inline void CALLBACK CollectionCallback(PTP_CALLBACK_INSTANCE instance, void*, PTP_TIMER timer)
{
    std::lock_guard lock(Submission::RecordingMutex());
    if (CollectLocked()) { Arm(timer); return; }
    const auto module = Timer().module;
    Timer() = {};
    CloseThreadpoolTimer(timer);
    FreeLibraryWhenCallbackReturns(instance, module);
}
inline void ScheduleCollectionLocked() noexcept
{
    if (Timer().timer) return;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&CollectionCallback), &module)) return;
    auto* timer = CreateThreadpoolTimer(&CollectionCallback, nullptr, nullptr);
    if (!timer) { FreeLibrary(module); return; } // Registry still retains every resource.
    Timer() = {timer, module}; Arm(timer);
}
inline void Collect()
{
    std::lock_guard lock(Submission::RecordingMutex());
    if (CollectLocked()) ScheduleCollectionLocked();
}

// Register ownership before any NR commands are appended. A failed later Record
// still leaves its observer attached, so partial private work is tracked safely.
inline std::shared_ptr<Lease> Attach(const std::shared_ptr<SessionOwner>& owner, void* job,
                                     Submission::ILogicalCommandList* logical)
{
    std::shared_ptr<Lease> lease;
    try
    {
        lease = std::make_shared<Lease>(owner, job, logical->Identity());
        Registry().push_back(lease);
    }
    catch (...)
    {
        // No command has been appended yet and no observer owns this token.
        owner->api.InvalidateRecording(owner->context, job);
        owner->api.CollectRecording(owner->context, job);
        return {};
    }
    if (FAILED(logical->ObserveRecording(lease)))
    {
        lease->Invalidated(lease->identity);
        return {};
    }
    return lease;
}
} // namespace DlssNr::Backend::LmxxfRecording
