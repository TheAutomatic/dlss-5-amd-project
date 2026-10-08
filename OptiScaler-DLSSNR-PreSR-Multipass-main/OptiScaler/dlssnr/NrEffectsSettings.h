#pragma once
#include <algorithm>
#include <cmath>
namespace DlssNr
{
inline float OverallIntensity(float value)
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 2.0f) : 1.0f;
}

// Host-only output controls. Neither these gains nor OverallIntensity enter
// a backend's network/history state or its environment/flags-file fallback.
struct ResidualSettings
{
    float lowGain = 1.f, detailGain = 1.f, skinProtection = 0.f, edgeProtection = 0.f;
    ResidualSettings Bounded() const
    {
        auto protection = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f; };
        return {OverallIntensity(lowGain), OverallIntensity(detailGain),
                protection(skinProtection), protection(edgeProtection)};
    }
    bool Active() const
    {
        const auto s = Bounded();
        return s.lowGain != 1.f || s.detailGain != 1.f || s.skinProtection != 0.f || s.edgeProtection != 0.f;
    }
};
static_assert(sizeof(ResidualSettings) == 16);
}
