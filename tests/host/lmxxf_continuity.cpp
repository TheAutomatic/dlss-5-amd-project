#include <cstdio>
#include <cstdlib>
#include <cstring>
#define LOG_WARN(...) ((void)0)
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/LmxxfFrameDiagnostics.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/submission/CommandListProxy.h"
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
    proxy.SetPipelineState1(nullptr); // No native list: exercise recording, no GPU command.
    proxy.DispatchRays(nullptr);
    proxy.BuildRaytracingAccelerationStructure(nullptr, 0, nullptr);
    proxy.SetPipelineState1(nullptr);
    const char *why=proxy.SplitRejectionReason();
    Require(std::strcmp(why,"|null_state_object||null_dispatch_rays||rtas|")==0, "all distinct blockers retained");
    Require(proxy.IsSplitIneligible(), "diagnostics never relax split safety");
    std::puts("continuity: PASS");
}
