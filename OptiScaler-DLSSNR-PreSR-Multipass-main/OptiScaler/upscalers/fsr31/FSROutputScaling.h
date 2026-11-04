#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace FSROutputScaling
{
struct Size
{
    uint32_t width = 0;
    uint32_t height = 0;
    bool operator==(const Size&) const = default;
};

inline float NormalizeMultiplier(float multiplier)
{
    // Invalid configuration must never reach a floating-point to integer conversion.
    if (!std::isfinite(multiplier) || multiplier <= 0.0f)
        return 1.0f;

    return std::clamp(multiplier, 0.5f, 3.0f);
}

inline bool IsValidSize(Size size, uint32_t limit)
{
    return size.width > 0 && size.height > 0 && size.width <= limit && size.height <= limit;
}

inline uint32_t ResolveDimension(uint32_t dimension, float multiplier, uint32_t limit)
{
    if (dimension == 0 || limit == 0)
        return 0;

    // Keep the existing float product and truncate only after multiplication. Compare
    // in double so even a limit of UINT32_MAX cannot round up before the bounds check.
    const double scaled = static_cast<float>(dimension) * NormalizeMultiplier(multiplier);
    if (scaled >= static_cast<double>(limit))
        return limit;
    if (scaled < 1.0)
        return 1;

    return static_cast<uint32_t>(scaled);
}

inline Size ResolveSize(Size size, float multiplier, uint32_t limit)
{
    return { ResolveDimension(size.width, multiplier, limit), ResolveDimension(size.height, multiplier, limit) };
}

struct ContextSizes
{
    Size target;
    Size maxRender;
    Size maxUpscale;
    float multiplier = 1.0f;
    bool valid = false;
};

inline ContextSizes ResolveContextSizes(Size display, Size render, bool outputScalingEnabled, bool lowResMV,
                                        bool extendedLimits, float multiplier, uint32_t dimensionLimit)
{
    ContextSizes sizes {};
    sizes.multiplier = NormalizeMultiplier(multiplier);
    if (!IsValidSize(display, dimensionLimit) || !IsValidSize(render, dimensionLimit))
        return sizes;

    const bool useOutputScaling = outputScalingEnabled && lowResMV;
    sizes.target = useOutputScaling ? ResolveSize(display, sizes.multiplier, dimensionLimit) : display;

    // Preserve the extended-limits contract: render above display at native size,
    // then let the external output scaler downsample when it is active.
    if (extendedLimits && render.width > display.width)
    {
        sizes.multiplier = 1.0f;
        sizes.maxRender = render;
        if (useOutputScaling)
            sizes.target = render;
    }
    else
    {
        sizes.maxRender = { std::max(sizes.target.width, display.width),
                            std::max(sizes.target.height, display.height) };
    }

    sizes.maxUpscale = sizes.target;
    sizes.valid = true;
    return sizes;
}

inline Size ResolveDispatchSize(Size target, Size contextMaximum, bool useOutputScaling, float multiplier,
                                std::optional<uint32_t> overrideWidth, std::optional<uint32_t> overrideHeight)
{
    // Parent pipeline allocations use target; the SDK allocations use contextMaximum.
    // Dynamic/partial requests can shrink either axis, but cannot grow past either.
    Size size { std::min(target.width, contextMaximum.width), std::min(target.height, contextMaximum.height) };
    if (size.width == 0 || size.height == 0)
        return {};

    if (useOutputScaling)
    {
        // Zero overrides are invalid requests: retain the configured size for that axis.
        if (overrideWidth.value_or(0) > 0)
            size.width = ResolveDimension(*overrideWidth, multiplier, size.width);
        if (overrideHeight.value_or(0) > 0)
            size.height = ResolveDimension(*overrideHeight, multiplier, size.height);
    }

    return size;
}
} // namespace FSROutputScaling
