// FSR-RR CPU contract tests: RR 1.2 ABI shape from the local compat header, and the
// upscaler-enum/ini-code routing that gates every creation path.
#define NOMINMAX
// OptiTypes.h references the logger in a parse warning path; this is a routing/ABI
// test, not a logging test, so the macro is stubbed for this TU only.
#define LOG_WARN(...) ((void) 0)
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/OptiTypes.h"
#undef LOG_WARN
#include <fsr-rr/ffx_denoiser.h>
#include <cassert>
#include <cstdio>

// OptiTypes.cpp's only external dependency is the GPU-info query, used for the FSR4-aware
// display names. This test does not exercise GPU identity, so the symbol is defined here
// instead of linking the IdentifyGpu translation unit and everything it drags in.
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/Config.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/misc/IdentifyGpu.h"
GpuInformation IdentifyGpu::getPrimaryGpu()
{
    return {};
}
#include <string>

int main()
{
    // ---- ABI shape (the values the RR 1.2 dispatch backend compiles against) ----
    assert(sizeof(ffxCreateContextDescDenoiser) == 40u);
    assert(sizeof(ffxDispatchDescDenoiser) == 448u);
    assert(sizeof(ffxDispatchDescDenoiserAmbientOcclusion) == 120u);
    assert(sizeof(ffxDispatchDescDenoiserDirectDiffuse) == 120u);
    assert(sizeof(ffxDispatchDescDenoiserDirectSpecular) == 120u);
    assert(sizeof(ffxDispatchDescDenoiserIndirectDiffuse) == 120u);
    assert(sizeof(ffxDispatchDescDenoiserIndirectSpecular) == 120u);
    assert(sizeof(ffxDispatchDescDenoiserSpecularOcclusion) == 120u);

    // Effect/sub-ID layout: dispatch is effect 0x00050000 + sub 0x41.
    assert(FFX_API_EFFECT_ID_DENOISER == 0x00050000u);
    assert(FFX_API_DISPATCH_DESC_TYPE_DENOISER == 0x00050041u);
    assert(FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER == 0x00050001u);
    assert(FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE == 0x00050021u);
    // Version this implementation accepts; a provider that reports anything else
    // must be rejected by DenoiserApiGenerationDx12, not silently driven.
    assert(FFX_DENOISER_VERSION_MAJOR == 1 && FFX_DENOISER_VERSION_MINOR == 2 && FFX_DENOISER_VERSION_PATCH == 0);

    // ---- Upscaler routing ----
    assert(CodeToUpscaler("fsr-rr") == Upscaler::FSR_RR);
    assert(UpscalerToCode(Upscaler::FSR_RR) == "fsr-rr");
    assert(IsFsr(Upscaler::FSR_RR));
    // Existing codes keep their meaning after the enum insertion.
    assert(CodeToUpscaler("fsr31") == Upscaler::FSR31);
    assert(CodeToUpscaler("dlssd") == Upscaler::DLSSD);
    assert(CodeToUpscaler("nonsense") == Upscaler::Reset);
    assert(UpscalerToCode(Upscaler::Reset).empty());
    // feature_version equality drives the V1_2 acceptance test in FfxApiProxy.
    const feature_version implemented(FFX_DENOISER_VERSION_MAJOR, FFX_DENOISER_VERSION_MINOR,
                                       FFX_DENOISER_VERSION_PATCH);
    const feature_version v12(1, 2, 0);
    const feature_version v11(1, 1, 0);
    assert(implemented == v12);
    assert(!(implemented == v11));

    std::puts("fsr_rr_abi: PASS");
    return 0;
}
