#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/amd/AmdPreSr.h"
#include <cassert>
#include <limits>
#include <iostream>

int main()
{
    using AmdPreSr::Settings;
    const Settings baseline;
    assert(baseline.SameHistorySettings(baseline));
    auto expectReset = [&](auto change) {
        Settings updated = baseline;
        change(updated);
        assert(!baseline.SameHistorySettings(updated));
        assert(!updated.SameHistorySettings(baseline));
    };
    expectReset([](Settings& s) { s.style = 1; });
    expectReset([](Settings& s) { s.toneCurve = 1; });
    expectReset([](Settings& s) { s.toneLift = .1f; });
    expectReset([](Settings& s) { s.useGameExposure = false; });
    expectReset([](Settings& s) { s.autoMask = false; });
    expectReset([](Settings& s) { s.toneChannels = true; });
    expectReset([](Settings& s) { s.tone = .5f; });
    expectReset([](Settings& s) { s.structure = .5f; });
    expectReset([](Settings& s) { s.skin = 0; });
    expectReset([](Settings& s) { s.everyFrame = true; });
    expectReset([](Settings& s) { s.encoding = 1; });
    expectReset([](Settings& s) { s.modelScale = .5f; });
    Settings outputOnly = baseline;
    outputOnly.look.exposureEV = 2;
    outputOnly.look.saturation = .5f;
    assert(baseline.SameHistorySettings(outputOnly));

    assert(AmdPreSr::BoundedToneLift(-1) == 0);
    assert(AmdPreSr::BoundedToneLift(.1f) == .1f);
    assert(AmdPreSr::BoundedToneLift(.5f) == .25f);
    assert(AmdPreSr::BoundedToneLift(std::numeric_limits<float>::infinity()) == 0);
    assert(AmdPreSr::BoundedToneLift(std::numeric_limits<float>::quiet_NaN()) == 0);
    Settings bounded = baseline, excessive = baseline;
    bounded.toneLift = .25f;
    excessive.toneLift = 100;
    assert(bounded.SameHistorySettings(excessive));
    std::cout << "amd_model_settings: PASS\n";
}
