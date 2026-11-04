// Link boundary for OptiTypes.cpp's display-only GPU query. Config INI
// load/save/profile/reset has no dependency on GPU discovery. Do not synthesize
// adapter properties: if that boundary becomes reachable, fail the regression
// instead of silently testing fabricated hardware behavior.
#define NOMINMAX
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/Config.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/misc/IdentifyGpu.h"
#include <cstdio>
#include <cstdlib>

GpuInformation IdentifyGpu::getPrimaryGpu()
{
    std::fputs("fsrd_ini_roundtrip: FAIL: GPU discovery reached CPU-only INI regression\n", stderr);
    std::abort();
}
