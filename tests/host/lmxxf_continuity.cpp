#include <cstdio>
#include <cstdlib>
#include <cstring>
#define LOG_WARN(...) ((void)0)
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfFrameDiagnostics.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/CommandListProxy.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/SplitEligibility.h"
void Require(bool ok, const char *s) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", s); std::exit(1); } }
int main()
{
    DlssNr::Backend::FrameDiagnostics d;
    unsigned logs = 0;
    for (unsigned i=0; i<10000; ++i) logs += d.Observe(i%2 == 0, i);
    Require(d.calls==10000 && d.recorded==5000 && d.original==5000, "alternating outcomes counted");
    Require(d.switches==9999 && d.maxRecorded==1 && d.maxOriginal==1, "flicker sequence counted");
    Require(logs==10, "high FPS alternating outcomes do not flood log");
    for (unsigned i=0; i<100; ++i) d.Observe(false, 10000+i);
    Require(d.streak==101 && d.maxOriginal==101, "continued original streak");
    Require(d.Observe(true, 20000), "late recovery reported");
    DlssNr::Submission::CommandListProxy proxy;
    Require(!DlssNr::Submission::ReadSplitEligibility(nullptr).allowed, "missing proxy fails closed");
    const auto initial = DlssNr::Submission::ReadSplitEligibility(&proxy);
    Require(initial.allowed, "eligible list is not rejected by diagnostic snapshot");
    proxy.SetPipelineState1(nullptr); // No native list: exercise recording, no GPU command.
    proxy.DispatchRays(nullptr);
    proxy.BuildRaytracingAccelerationStructure(nullptr, 0, nullptr);
    proxy.SetPipelineState1(nullptr);
    const char *why=proxy.SplitRejectionReason();
    Require(std::strcmp(why,"|null_state_object||null_dispatch_rays||rtas|")==0, "all distinct blockers retained");
    Require(proxy.IsSplitIneligible(), "diagnostics never relax split safety");
    const auto blocked = DlssNr::Submission::ReadSplitEligibility(&proxy);
    Require(!blocked.allowed && blocked.reason == "|null_state_object||null_dispatch_rays||rtas|",
            "pre-record snapshot retains all blockers");
    // Later queries rewrite the proxy's borrowed diagnostic string. Saved reasons must
    // remain stable after that, after the temporary QI reference, and after final Release.
    proxy.SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    const auto changed = DlssNr::Submission::ReadSplitEligibility(&proxy);
    Require(changed.reason != blocked.reason, "new blocker exercises diagnostic storage mutation");
    Require(blocked.reason == "|null_state_object||null_dispatch_rays||rtas|", "saved reason owns its storage");
    auto *owned = new DlssNr::Submission::CommandListProxy;
    owned->BuildRaytracingAccelerationStructure(nullptr, 0, nullptr);
    const auto retained = DlssNr::Submission::ReadSplitEligibility(owned);
    owned->Release();
    Require(!retained.allowed && retained.reason == "|rtas|", "reason survives final proxy release; RTAS guard retained");
    std::puts("continuity: PASS");
}
