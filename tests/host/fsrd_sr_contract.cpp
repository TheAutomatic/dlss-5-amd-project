#define NOMINMAX
#include <ffx_api.h>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/upscalers/fsr31/FSROutputScaling.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/upscalers/fsr31/FSRInputAlignment.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/upscalers/fsr31/FSRUpscaleSettings.h"
#include <cassert>
#include <cstdio>
#include <limits>

int main()
{
    using namespace FSROutputScaling;
    for (float multiplier : { .75f, 1.5f, 2.5f })
    {
        const auto sizes = ResolveContextSizes({1920,1080}, {1280,720}, true, true, false, multiplier, 16384);
        const Size expected {uint32_t(1920 * multiplier), uint32_t(1080 * multiplier)};
        assert(sizes.valid && sizes.target == expected);
        assert(ResolveDispatchSize(sizes.target, sizes.maxUpscale, true, multiplier, 1920, 1080) == expected);
        assert(ResolveDispatchSize(sizes.target, sizes.maxUpscale, true, multiplier, 99999, 99999) == expected);
    }
    assert(ResolveDimension(101, .75f, 16384) == 75);
    assert(ResolveDimension(101, 1.5f, 16384) == 151);
    assert(NormalizeMultiplier(std::numeric_limits<float>::quiet_NaN()) == 1.f);
    assert(!ResolveContextSizes({0,1080},{1280,720},true,true,false,1.5f,16384).valid);
    const auto extended = ResolveContextSizes({1920,1080},{2560,1440},true,true,true,2.5f,16384);
    assert(extended.multiplier == 1 && extended.target == Size({2560,1440}));

    using namespace FSRInputAlignment;
    const auto forwarded = ResolveMotionRegion({0,0},{640,360},{1280,720},false,640,360);
    assert(!forwarded.displayResolution && forwarded.extent.width == 640);
    assert(ValidateSRRegions({}, {}, true, forwarded, true, true) == Refusal::MotionResolution);
    assert(ValidateSRRegions({}, {}, true, forwarded, true, false) == Refusal::None);
    const auto offset = ResolveMotionRegion({10,2},{640,360},{1280,720},true,650,362);
    assert(ValidateSRRegions({}, {}, true, offset, true, false) == Refusal::MotionOrigin);
    assert(ValidateSRRegions({3,4},{},true,forwarded,false,false) == Refusal::ColorOrigin);
    assert(ValidateSRRegions({3,4},{},true,forwarded,true,false) == Refusal::None);
    assert(ValidateSRRegions({}, {2,3}, true, forwarded, false, false) == Refusal::DepthOrigin);
    assert(ValidateSRRegions({}, {2,3}, true, forwarded, true, false) == Refusal::None);
    assert(!CoversRegion(640,360,{UINT32_MAX,0},{640,360}));

    FSR31::UpscaleSettings settings;
    ffxContext context = nullptr;
    unsigned calls = 0;
    ffxReturnCode_t response = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    const auto configure = [&](ffxContext*, const ffxConfigureDescHeader*) { ++calls; return response; };
    const auto apply = [&](float desired) {
        return FSR31::ApplyUpscaleSetting(&context, settings.velocity, desired,
            FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR, configure);
    };
    apply(.25f);
    assert(!settings.velocity.hasAppliedValue);
    response = FFX_API_RETURN_OK;
    apply(.25f);
    assert(calls == 2 && settings.velocity.hasAppliedValue && settings.velocity.appliedValue == .25f);
    apply(.25f);
    assert(calls == 2);
    response = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    apply(.5f);
    assert(settings.velocity.appliedValue == .25f);
    response = FFX_API_RETURN_OK;
    apply(.75f);
    assert(calls == 4 && settings.velocity.appliedValue == .75f);
    settings.ResetForContext();
    apply(.75f);
    assert(calls == 5);
    response = FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    apply(.9f);
    assert(settings.velocity.unsupported);
    apply(.8f);
    assert(calls == 6);
    settings.ResetForContext();
    response = FFX_API_RETURN_OK;
    apply(.8f);
    assert(calls == 7 && settings.velocity.appliedValue == .8f);
    std::puts("fsrd_sr_contract: PASS (fractional dimensions, regions, transient settings and context reset)");
}
