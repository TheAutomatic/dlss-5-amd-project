#pragma once
#include <cstdint>

namespace FSRInputAlignment
{
struct Origin
{
    uint32_t x = 0;
    uint32_t y = 0;
};

struct Extent
{
    uint32_t width = 0;
    uint32_t height = 0;
};

inline bool CoversRegion(uint64_t resourceWidth, uint32_t resourceHeight, Origin origin, Extent extent) noexcept
{
    return extent.width != 0 && extent.height != 0 &&
           uint64_t(origin.x) + extent.width <= resourceWidth &&
           uint64_t(origin.y) + extent.height <= resourceHeight;
}

enum class MotionCorrection
{
    None,
    ZeroOrigin,
    RenderResolution
};

struct MotionRegion
{
    Origin origin;
    Extent extent;
    bool displayResolution;
    MotionCorrection correction = MotionCorrection::None;
};

// Streamline can forward an already localized MV texture with stale NGX origin
// or resolution metadata. Resolve that exception identically for RR and SR;
// a resource that still covers the declared region retains its declared origin.
inline MotionRegion ResolveMotionRegion(Origin declaredOrigin, Extent render, Extent display, bool lowResMotion,
                                        uint64_t resourceWidth, uint32_t resourceHeight) noexcept
{
    MotionRegion result { declaredOrigin, lowResMotion ? render : display, !lowResMotion };
    if (!CoversRegion(resourceWidth, resourceHeight, result.origin, result.extent))
    {
        if (CoversRegion(resourceWidth, resourceHeight, {}, result.extent))
        {
            result.origin = {};
            result.correction = MotionCorrection::ZeroOrigin;
        }
        else if (result.displayResolution && CoversRegion(resourceWidth, resourceHeight, {}, render))
        {
            result.origin = {};
            result.extent = render;
            result.displayResolution = false;
            result.correction = MotionCorrection::RenderResolution;
        }
    }
    return result;
}

enum class Refusal
{
    None,
    ColorOrigin,
    DepthOrigin,
    MotionOrigin,
    MotionResolution
};

inline const char* RefusalName(Refusal refusal) noexcept
{
    switch (refusal)
    {
    case Refusal::ColorOrigin: return "nonzero color origin";
    case Refusal::DepthOrigin: return "nonzero encoded depth origin";
    case Refusal::MotionOrigin: return "nonzero motion-vector origin";
    case Refusal::MotionResolution: return "motion-vector resolution differs from SR context";
    default: return "none";
    }
}

// FFX SR descriptors have no input-origin fields. RR composition localizes color
// and re-encodes depth; original-color paths must already be zero-origin.
inline Refusal ValidateSRRegions(Origin colorOrigin, Origin depthOrigin, bool hasDepth,
                                const MotionRegion& motion, bool composedColor,
                                bool contextDisplayMotion) noexcept
{
    if (!composedColor && (colorOrigin.x != 0 || colorOrigin.y != 0))
        return Refusal::ColorOrigin;
    if (!composedColor && hasDepth && (depthOrigin.x != 0 || depthOrigin.y != 0))
        return Refusal::DepthOrigin;
    if (motion.origin.x != 0 || motion.origin.y != 0)
        return Refusal::MotionOrigin;
    if (motion.displayResolution != contextDisplayMotion)
        return Refusal::MotionResolution;
    return Refusal::None;
}
} // namespace FSRInputAlignment
