#include <Windows.h>
#include "../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/SubmissionState.h"
#include <cassert>
#include <iostream>

int main()
{
    AmdPreSr::SubmissionState state;
    assert(!state.BlocksRecord());
    assert(!state.ReportStall(6000));
    // Timestamp zero is valid; admission must still protect its pending job.
    state.Record(0);
    assert(state.BlocksRecord());
    assert(state.ReportStall(6000));
    assert(state.BlocksRecord());
    state.Submit(6001);
    assert(!state.BlocksRecord()); // Other slots may now Record without waiting for HIP/GPU.
    assert(!state.CanRetire(true, 0, 0)); // A missing fence signal is not completion.
    // Resume after an old frame / menu pause: the new unsubmitted list must
    // use its own timestamp, even if a previous submission was hours ago.
    state.Record(100);
    state.Submit(120);
    assert(state.CanRetire(true, 1, 1));
    state.Record(60000);
    assert(state.BlocksRecord());
    assert(!state.ReportStall(60001));
    assert(!state.CanRetire(true, 1, 1));

    // A delayed submission remains owned and can still be published later.
    assert(state.ReportStall(65001));
    assert(state.BlocksRecord());
    assert(!state.submitted);
    assert(!state.ReportStall(65002));
    assert(!state.CanRetire(true, 100, 1));
    state.Submit(66000);
    assert(!state.BlocksRecord());
    assert(!state.ReportStall(66001));
    assert(!state.CanRetire(true, 1, 2)); // HIP done, D3D still uses the list.
    assert(!state.CanRetire(false, 2, 2)); // D3D fallback finished, HIP still running.
    assert(state.CanRetire(true, 2, 2));

    // Device loss must never masquerade as an exceptionally advanced fence.
    assert(!state.CanRetire(true, UINT64_MAX, 2));

    // Staging rejected NR, but B's GPU copy/conversion commands still exist.
    state.Record(70000);
    assert(!state.CanRetire(true, 2, 2));
    state.Submit(70001);
    assert(!state.CanRetire(true, 2, 3));
    assert(state.CanRetire(true, 3, 3));

    // A long submitted job can recover; reporting alone does not retire it.
    state.Record(80000);
    state.Submit(80001);
    assert(state.ReportStall(86002));
    assert(state.submitted);
    assert(!state.CanRetire(false, 3, 4));
    assert(state.CanRetire(true, 4, 4));
    std::cout << "submission-state regression scenarios passed\n";
}
