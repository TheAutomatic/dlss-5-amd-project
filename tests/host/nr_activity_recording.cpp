#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfRecordingOwner.h"
#include <cassert>
#include <iostream>

namespace Recording = DlssNr::Backend::LmxxfRecording;
namespace Submission = DlssNr::Submission;
static int32_t enqueueResult = LMXXF_NR_OK;
static int32_t endResult = LMXXF_NR_OK;
static bool recovered = false;

static std::shared_ptr<Recording::SessionOwner> Owner()
{
    auto owner = std::make_shared<Recording::SessionOwner>();
    owner->api.BeginRecordingExecution = [](void*, void*, void*) -> int32_t { return LMXXF_NR_OK; };
    owner->api.EnqueueHip = [](void*, void*, void*) { return enqueueResult; };
    owner->api.GetLastError = [](char* buffer, uint32_t) -> int32_t {
        buffer[0] = recovered ? 'R' : '\0';
        buffer[1] = '\0';
        return LMXXF_NR_OK;
    };
    owner->api.EndRecordingExecution = [](void*, void*, void*, uint32_t, void*, uint64_t, int32_t) {
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
    std::cout << "NR activity: real submission, diagnostics, failures and retired owners: PASS\n";
}
