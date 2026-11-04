#pragma once

#include <cstdint>

namespace FSRD
{
// Uses the immutable native/effective extent fields of a captured Streamline tag.
// A missing extent is the whole native resource; an explicit extent must describe
// a nonempty region entirely inside that resource. Promote before adding bases so
// malformed tags cannot pass through 32-bit coordinate wraparound.
template <class Diagnostic>
inline bool HasValidTaggedResourceExtent(const Diagnostic& diagnostic) noexcept
{
    if (diagnostic.nativeWidth == 0 || diagnostic.nativeHeight == 0 ||
        diagnostic.effectiveWidth == 0 || diagnostic.effectiveHeight == 0)
        return false;

    if (!diagnostic.usesExtent)
    {
        const uint32_t nativeEffectiveWidth = diagnostic.nativeWidth > UINT32_MAX
            ? UINT32_MAX : static_cast<uint32_t>(diagnostic.nativeWidth);
        return diagnostic.extentLeft == 0 && diagnostic.extentTop == 0 &&
            diagnostic.effectiveWidth == nativeEffectiveWidth &&
            diagnostic.effectiveHeight == diagnostic.nativeHeight;
    }

    return uint64_t(diagnostic.extentLeft) + uint64_t(diagnostic.effectiveWidth) <=
            diagnostic.nativeWidth &&
        uint64_t(diagnostic.extentTop) + uint64_t(diagnostic.effectiveHeight) <=
            diagnostic.nativeHeight;
}

// Linear depth consumes the render region starting at the tag's base. Its
// logical coverage may be larger than that region; exact-size signal contracts
// such as AO and specular hit distance retain their own stricter checks.
template <class Diagnostic>
inline bool TaggedResourceExtentCovers(const Diagnostic& diagnostic,
                                      uint32_t width, uint32_t height) noexcept
{
    return width != 0 && height != 0 && HasValidTaggedResourceExtent(diagnostic) &&
        diagnostic.effectiveWidth >= width && diagnostic.effectiveHeight >= height;
}
}
