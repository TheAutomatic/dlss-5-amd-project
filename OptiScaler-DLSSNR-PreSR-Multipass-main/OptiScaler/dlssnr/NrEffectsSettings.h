#pragma once
#include <algorithm>
#include <cmath>
namespace DlssNr
{
inline float OverallIntensity(float value)
{
    return std::isfinite(value) ? std::clamp(value, 0.0f, 2.0f) : 1.0f;
}
}
