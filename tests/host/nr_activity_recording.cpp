#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfRecordingOwner.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <cstring>

namespace Recording = DlssNr::Backend::LmxxfRecording;
namespace Submission = DlssNr::Submission;
static int32_t enqueueResult = LMXXF_NR_OK;
static int32_t endResult = LMXXF_NR_OK;
static bool recovered = false;
static int32_t beginResult = LMXXF_NR_OK;
static thread_local const char* callbackError = "";
static unsigned failureReports = 0;
static unsigned beginCalls = 0, enqueueCalls = 0, endCalls = 0;
static void* endQueue = nullptr;
static void* endFence = nullptr;
static uint32_t endFlags = 0;
static uint64_t endValue = 0;
static int32_t endStatus = S_OK;

static std::shared_ptr<Recording::SessionOwner> Owner()
{
    auto owner = std::make_shared<Recording::SessionOwner>();
    owner->api.BeginRecordingExecution = [](void*, void*, void*) -> int32_t { ++beginCalls; return beginResult; };
    owner->api.EnqueueHip = [](void*, void*, void*) { ++enqueueCalls; return enqueueResult; };
    owner->api.GetLastError = [](char* buffer, uint32_t size) -> int32_t {
        std::snprintf(buffer, size, "%s", recovered ? "R" : callbackError);
        return LMXXF_NR_OK;
    };
    owner->onFailure = [](const Recording::SessionOwner::Failure&) noexcept { ++failureReports; };
    owner->api.EndRecordingExecution = [](void*, void*, void* queue, uint32_t flags, void* fence,
                                          uint64_t value, int32_t status) {
        ++endCalls; endQueue = queue; endFlags = flags; endFence = fence; endValue = value; endStatus = status;
        return endResult;
    };
    return owner;
}

static void Execute(Recording::Lease& lease, bool consumer = true)
{
    Submission::RecordingExecution execution;
    execution.identity = lease.identity;
    execution.status = S_OK;
    execution.producerSubmitted = true;
    execution.continuationSubmitted = consumer;
    assert(SUCCEEDED(lease.BeforeExecute(execution)));
    lease.Between(execution);
    lease.Executed(execution);
}

static void DirectExecutionContracts()
{
    beginResult = enqueueResult = endResult = LMXXF_NR_OK;
    recovered = false;
    auto owner = Owner();
    owner->coverage = std::make_shared<DlssNr::Coverage::Counters>();
    Recording::Lease direct(owner, nullptr, { 10, 1 });
    direct.directRecording = direct.ready = direct.neural = true;
    Submission::RecordingExecution execution {};
    execution.identity = direct.identity;
    execution.serial = 100;
    execution.queue = reinterpret_cast<ID3D12CommandQueue*>(uintptr_t(0x1000));
    execution.fence = reinterpret_cast<ID3D12Fence*>(uintptr_t(0x2000));
    execution.fenceValue = 37;
    execution.status = S_OK;
    execution.producerSubmitted = true;
    execution.continuationSubmitted = false;
    const auto beginsBefore = beginCalls, enqueuesBefore = enqueueCalls, endsBefore = endCalls;
    assert(SUCCEEDED(direct.BeforeExecute(execution)));
    assert(beginCalls == beginsBefore + 1 && direct.begun);
    direct.Between(execution);
    assert(enqueueCalls == enqueuesBefore && !direct.enqueued);

    // A rejected overlapping serial must not finish the still-active game batch.
    auto overlap = execution;
    overlap.serial = 101;
    assert(direct.BeforeExecute(overlap) == E_PENDING);
    overlap.status = E_PENDING;
    overlap.producerSubmitted = false;
    direct.Executed(overlap);
    assert(endCalls == endsBefore && direct.begun && direct.activeExecutionSerial == execution.serial);
    assert(beginCalls == beginsBefore + 1);

    direct.Executed(execution);
    assert(endCalls == endsBefore + 1 && !direct.begun);
    assert(endQueue == execution.queue && endFence == execution.fence);
    assert(endValue == execution.fenceValue && endStatus == execution.status);
    assert(endFlags == (LMXXF_NR_SUBMITTED_PRODUCER | LMXXF_NR_SUBMITTED_CONSUMER));
    assert(owner->coverage->directAccepted == 1 && owner->coverage->splitAccepted == 0);
    assert(owner->coverage->enqueues == 0 && owner->activity.IsRunning());

    // RecordDirect failed after writing commands: Attach retains the observer,
    // but ready stays false. Preserve the game batch and its real tail proof
    // without certifying an NR output or enqueueing a second submission.
    auto partialOwner = Owner();
    partialOwner->coverage = std::make_shared<DlssNr::Coverage::Counters>();
    Recording::Lease partial(partialOwner, nullptr, { 12, 1 });
    partial.directRecording = partial.neural = true;
    execution.identity = partial.identity;
    execution.serial = 150;
    assert(SUCCEEDED(partial.BeforeExecute(execution)));
    partial.Between(execution);
    partial.Executed(execution);
    assert(endFlags == LMXXF_NR_SUBMITTED_PRODUCER);
    assert(endQueue == execution.queue && endFence == execution.fence && endValue == execution.fenceValue);
    assert(!partialOwner->failed && !partialOwner->activity.IsRunning());
    assert(partialOwner->coverage->directAccepted == 0 && enqueueCalls == enqueuesBefore);

    // The actual failed tail Signal status goes to End unchanged, poisoning retirement.
    auto failedOwner = Owner();
    failedOwner->coverage = std::make_shared<DlssNr::Coverage::Counters>();
    Recording::Lease failed(failedOwner, nullptr, { 11, 1 });
    failed.directRecording = failed.ready = failed.neural = true;
    execution.identity = failed.identity;
    execution.serial = 200;
    execution.fenceValue = 91;
    assert(SUCCEEDED(failed.BeforeExecute(execution)));
    failed.Between(execution);
    execution.status = E_FAIL;
    endResult = LMXXF_NR_UNAVAILABLE;
    failed.Executed(execution);
    assert(endQueue == execution.queue && endFence == execution.fence && endValue == 91 && endStatus == E_FAIL);
    assert(endFlags == (LMXXF_NR_SUBMITTED_PRODUCER | LMXXF_NR_SUBMITTED_CONSUMER));
    assert(failed.runtimeRetirementUncertain && failedOwner->failed && !failedOwner->activity.IsRunning());
    assert(failedOwner->coverage->directAccepted == 0 && failedOwner->coverage->splitAccepted == 0);
    assert(enqueueCalls == enqueuesBefore);
    endResult = LMXXF_NR_OK;
}

int main()
{
    auto owner = Owner();
    Recording::Lease lease(owner, nullptr, { 1, 1 });
    lease.ready = lease.neural = true;
    assert(!owner->activity.IsRunning());
    Execute(lease);
    assert(owner->activity.IsRunning());

    owner->activity.Reset();
    Execute(lease);
    assert(!owner->activity.IsRunning());
    Recording::Lease fresh(owner, nullptr, { 1, 2 });
    fresh.ready = fresh.neural = true;
    Execute(fresh);
    assert(owner->activity.IsRunning());

    auto replacement = Owner();
    Execute(fresh);
    assert(!replacement->activity.IsRunning());
    Recording::Lease diagnostic(replacement, nullptr, { 2, 1 });
    diagnostic.ready = true;
    Execute(diagnostic);
    assert(!replacement->activity.IsRunning());

    Recording::Lease partial(replacement, nullptr, { 2, 2 });
    partial.ready = partial.neural = true;
    Execute(partial, false);
    assert(!replacement->activity.IsRunning());

    recovered = true;
    Execute(fresh);
    assert(!owner->activity.IsRunning());
    recovered = false;
    Recording::Lease failed(replacement, nullptr, { 2, 3 });
    failed.ready = failed.neural = true;
    enqueueResult = LMXXF_NR_INVALID_ARGUMENT;
    Execute(failed);
    assert(replacement->failed && !replacement->activity.IsRunning());

    enqueueResult = LMXXF_NR_OK;
    auto endFailedOwner = Owner();
    Recording::Lease endFailed(endFailedOwner, nullptr, { 3, 1 });
    endFailed.ready = endFailed.neural = true;
    endResult = LMXXF_NR_INVALID_ARGUMENT;
    Execute(endFailed);
    assert(endFailedOwner->failed && !endFailedOwner->activity.IsRunning());
    assert(std::strcmp(endFailedOwner->failure.phase, "EndRecordingExecution") == 0);

    // The menu runs on another thread: preserve the original TLS error, not
    // whatever GetLastError happens to contain on the later reader's thread.
    endResult = LMXXF_NR_OK;
    auto beginFailedOwner = Owner();
    Recording::Lease beginFailed(beginFailedOwner, nullptr, { 4, 1 });
    beginFailed.ready = beginFailed.neural = true;
    beginResult = LMXXF_NR_INVALID_ARGUMENT;
    const auto reportsBefore = failureReports;
    std::thread submit([&] {
        std::lock_guard lock(Submission::RecordingMutex());
        callbackError = "Begin queue: device mismatch";
        Submission::RecordingExecution execution;
        execution.identity = beginFailed.identity;
        assert(FAILED(beginFailed.BeforeExecute(execution)));
        execution.status = E_FAIL;
        beginFailed.Executed(execution);
        callbackError = "unrelated later error";
    });
    submit.join();
    assert(callbackError[0] == '\0');
    assert(beginFailedOwner->failed && failureReports == reportsBefore + 1);
    assert(std::strcmp(beginFailedOwner->failure.phase, "BeginRecordingExecution") == 0);
    assert(std::strcmp(beginFailedOwner->failure.error.data(), "Begin queue: device mismatch") == 0);
    assert(beginFailedOwner->failure.result == LMXXF_NR_INVALID_ARGUMENT);

    // Close/fence creation can fail before Begin is called at all.
    auto rejectedOwner = Owner();
    Recording::Lease rejected(rejectedOwner, nullptr, { 5, 1 });
    Submission::RecordingExecution execution;
    execution.identity = rejected.identity;
    execution.status = E_OUTOFMEMORY;
    rejected.Executed(execution);
    assert(!rejectedOwner->failed && !rejectedOwner->activity.IsRunning());
    assert(std::strcmp(rejectedOwner->failure.phase, "ExecuteCommandLists") == 0);
    assert(rejectedOwner->failure.result == E_OUTOFMEMORY);
    DirectExecutionContracts();
    std::cout << "NR activity: real submission, diagnostics, failures and retired owners: PASS\n";
}
