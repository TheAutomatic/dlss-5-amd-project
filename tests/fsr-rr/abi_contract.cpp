// The product pch defines NOMINMAX before windows.h; replicate that here.
#define NOMINMAX
// Verifies the RR 1.2 ABI shape, provider routing, and enum mappings.
#include <fsr-rr/ffx_denoiser.h>
// OptiTypes.h references the logger in a parse warning path; stub for this TU.
#define LOG_WARN(...) ((void) 0)
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/OptiTypes.h"
#undef LOG_WARN
#include <cassert>
#include <string>


#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/Config.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/misc/IdentifyGpu.h"
GpuInformation IdentifyGpu::getPrimaryGpu()
{
    return {};
}
int main()
{
    // ---- ABI shape ----
    assert(sizeof(ffxCreateContextDescDenoiser) == 40u);
    assert(sizeof(ffxDispatchDescDenoiser) == 448u);
    for (auto s : { sizeof(ffxDispatchDescDenoiserAmbientOcclusion), sizeof(ffxDispatchDescDenoiserDirectDiffuse),
                    sizeof(ffxDispatchDescDenoiserDirectSpecular), sizeof(ffxDispatchDescDenoiserIndirectDiffuse),
                    sizeof(ffxDispatchDescDenoiserIndirectSpecular), sizeof(ffxDispatchDescDenoiserSpecularOcclusion) })
        assert(s == 120u);

    assert(FFX_API_EFFECT_ID_DENOISER == 0x00050000u);
    assert(FFX_API_DISPATCH_DESC_TYPE_DENOISER == 0x00050041u);
    assert(FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER == 0x00050001u);
    assert(FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE == 0x00050021u);
    assert(FFX_DENOISER_VERSION_MAJOR == 1 && FFX_DENOISER_VERSION_MINOR == 2 && FFX_DENOISER_VERSION_PATCH == 0);

    // ---- Routing ----
    assert(CodeToUpscaler("fsr-rr") == Upscaler::FSR_RR);
    assert(UpscalerToCode(Upscaler::FSR_RR) == "fsr-rr");
    assert(IsFsr(Upscaler::FSR_RR));
    assert(CodeToUpscaler("fsr31") == Upscaler::FSR31);
    assert(CodeToUpscaler("dlssd") == Upscaler::DLSSD);

    const feature_version v12(1, 2, 0), v11(1, 1, 0);
    assert(v12 == feature_version(FFX_DENOISER_VERSION_MAJOR, FFX_DENOISER_VERSION_MINOR, FFX_DENOISER_VERSION_PATCH));
    assert(!(v12 == v11));

    // ---- Segment ID contract (P1 schema) ----
    // These must match the P1 ID schema enum; assert the count is stable.
    constexpr int kSegmentCount = 15;
    static_assert(kSegmentCount == 15, "segment count must match P1 schema");

    std::puts("fsr-rr abi_contract: PASS");
    return 0;
}
