#pragma once
#include <cstdint>
#include <limits>
#include <optional>
#include <nvsdk_ngx.h>

namespace DlssNr
{
struct SrOutputExtent
{
    unsigned int width = 0, height = 0;
};

// Evaluate parameters may retain GetOptimalSettings results: OutWidth/OutHeight
// can then describe the render size, not the image SR just wrote. Use the
// current feature's display extent, or the explicit FSR per-frame output size.
// Only callers without a feature extent fall back to the output allocation.
template <typename Parameters>
std::optional<SrOutputExtent> ResolveSrOutputExtent(const Parameters* params, SrOutputExtent feature,
                                                   uint64_t allocationWidth, unsigned int allocationHeight)
{
    if (!allocationWidth || !allocationHeight || allocationWidth > (std::numeric_limits<unsigned int>::max)())
        return {};
    unsigned int dynamicWidth = 0, dynamicHeight = 0;
    const bool haveWidth = params->Get("FSR.upscaleSize.width", &dynamicWidth) == NVSDK_NGX_Result_Success;
    const bool haveHeight = params->Get("FSR.upscaleSize.height", &dynamicHeight) == NVSDK_NGX_Result_Success;
    if ((haveWidth && dynamicWidth) || (haveHeight && dynamicHeight))
    {
        if (!haveWidth || !haveHeight || !dynamicWidth || !dynamicHeight)
            return {};
        feature = {dynamicWidth, dynamicHeight};
    }
    if (!feature.width && !feature.height)
        feature = {static_cast<unsigned int>(allocationWidth), allocationHeight};
    if (!feature.width || !feature.height || feature.width > allocationWidth || feature.height > allocationHeight)
        return {};
    return feature;
}
} // namespace DlssNr
