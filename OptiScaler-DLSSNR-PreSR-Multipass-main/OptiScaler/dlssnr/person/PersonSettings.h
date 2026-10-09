#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace DlssNr::Person
{
// Host-owned output controls. They never change the backend's network settings.
struct Settings
{
    float strength = 1.f;
    float detail = 1.f;
    Settings Bounded() const
    {
        auto clamp = [](float value) { return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 1.f; };
        return {clamp(strength), clamp(detail)};
    }
    bool ChangesSinglePass() const { auto s = Bounded(); return s.strength != 1.f || s.detail != 1.f; }
    bool operator==(const Settings&) const = default;
};

// Fade only the last 50 ms of an already valid result. Invalid/expired masks
// still bypass immediately; this never extends their accepted lifetime.
inline float MaskFreshness(uint64_t ageMs)
{
    if (ageMs <= 200) return 1.f;
    if (ageMs >= 250) return 0.f;
    const float t = float(ageMs - 200) / 50.f;
    return 1.f - t * t * (3.f - 2.f * t);
}
}
