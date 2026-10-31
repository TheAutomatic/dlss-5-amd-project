#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/MochizukiPrepareRetry.h"
#include <cassert>
#include <cstdio>

int main()
{
    using namespace DlssNr::Backend;
    using Outcome=MochizukiPrepareOutcome;
    constexpr auto budget="insufficient VRAM for 2232x1256 (need 1835 MB, free 0 MB of a 12484 MB budget)";
    assert(ClassifyMochizukiPrepare(LMXXF_NR_UNAVAILABLE,budget)==Outcome::VramWait);
    assert(ClassifyMochizukiPrepare(LMXXF_NR_OK,budget)==Outcome::Ready);
    assert(ClassifyMochizukiPrepare(LMXXF_NR_INVALID_ARGUMENT,budget)==Outcome::Failed);
    assert(ClassifyMochizukiPrepare(LMXXF_NR_UNAVAILABLE,"colour format 999 is not supported")==Outcome::Failed);
    assert(ClassifyMochizukiPrepare(LMXXF_NR_UNAVAILABLE,"unrelated failure mentioning insufficient VRAM for x")==Outcome::Failed);
    constexpr auto candidate="candidate frame buffers failed at 2568x1448: insufficient VRAM for 2568x1448 (need 248 MB, free 232 MB of a 11741 MB budget)";
    assert(ClassifyMochizukiPrepare(LMXXF_NR_UNAVAILABLE,candidate)==Outcome::VramWait);
    assert(ClassifyMochizukiPrepare(LMXXF_NR_INVALID_ARGUMENT,candidate)==Outcome::Failed);
    assert(ClassifyMochizukiPrepare(LMXXF_NR_UNAVAILABLE,"candidate frame buffers failed at 2568x1448: invalid resource")==Outcome::Failed);
    assert(ClassifyMochizukiPrepare(LMXXF_NR_UNAVAILABLE,"candidate frame buffers failed at 2568x1448")==Outcome::Failed);
    MochizukiPrepareFailureGate gate;
    // Reproduce a long budget hold, well beyond the previous 50-frame trip.
    for (unsigned i=0;i<2000;++i) {
        gate.Observe(ClassifyMochizukiPrepare(LMXXF_NR_UNAVAILABLE,budget));
        assert(!gate.Open() && gate.Failures()==0);
    }
    // PBZ: admission succeeds, but candidate buffers are refused. The runtime
    // retries after its OOM hold; this must survive more than 50 host calls too.
    for (unsigned i=0;i<2000;++i) {
        gate.Observe(ClassifyMochizukiPrepare(LMXXF_NR_UNAVAILABLE,candidate));
        assert(!gate.Open() && gate.Failures()==0);
    }
    // A smaller input or recovered budget can reach Prepare and resume normally.
    gate.Observe(ClassifyMochizukiPrepare(LMXXF_NR_OK,{}));
    assert(!gate.Open());
    for (unsigned i=0;i<49;++i) gate.Observe(Outcome::Failed);
    assert(!gate.Open() && gate.Failures()==49);
    gate.Observe(Outcome::VramWait);
    assert(!gate.Open() && gate.Failures()==0);
    // Fatal/unknown failures still stop at 50; do not weaken the leak guard.
    for (unsigned i=0;i<50;++i) gate.Observe(Outcome::Failed);
    assert(gate.Open() && gate.Failures()==50);
    gate.Observe(Outcome::Failed);
    assert(gate.Open() && gate.Failures()==50);
    gate.Reset();
    assert(!gate.Open());
    for (auto wait : {Outcome::Building,Outcome::Settling}) {
        for (unsigned i=0;i<49;++i) gate.Observe(Outcome::Failed);
        gate.Observe(wait);
        assert(!gate.Open() && gate.Failures()==0);
    }
    assert(ClassifyMochizukiPrepare(LMXXF_NR_UNAVAILABLE,"building the network")==Outcome::Building);
    assert(ClassifyMochizukiPrepare(LMXXF_NR_UNAVAILABLE,"input resolution is settling")==Outcome::Settling);
    std::puts("Mochizuki prepare retry: PASS (VRAM recovery, waits, fatal 50-frame breaker)");
}
