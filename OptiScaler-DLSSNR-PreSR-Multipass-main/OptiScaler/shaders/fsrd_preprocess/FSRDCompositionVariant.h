#pragma once
#include <cmath>
#include <cstdint>

namespace FSRD::Composition
{
    enum class PipelineVariant : uint32_t { Generic, Light, NoRecovery };

    // The two shaders classify actual whole tiles from the same original weights.
    // CPU selection only establishes a configuration where both methods exist.
    inline bool CanSplitTiles(uint32_t flags, float detail,
                              uint32_t recoveryMask, uint32_t lightMask) noexcept
    {
        if ((flags & ((1u << 16) | 1u)) != 0 || !std::isfinite(detail) || detail <= 0.0f)
            return false;
        if ((recoveryMask & ~7u) != 0 || (lightMask & ~7u) != 0) return false;
        return (recoveryMask & lightMask) != 0 && (recoveryMask & ~lightMask) != 0;
    }

    // Keep the generic shader for every diagnostic or unproven configuration.
    // RecoveryWeights uses bits 0/1/2 for Flat/Specular/Diffuse. A selected
    // class can reach Anchor exactly when its bit is absent from the Light mask.
    inline PipelineVariant ChoosePipeline(uint32_t flags, float detail,
                                          uint32_t recoveryMask, uint32_t lightMask) noexcept
    {
        if ((flags & ((1u << 16) | 1u)) != 0) return PipelineVariant::Generic;
        if (detail <= 0.0f || recoveryMask == 0) return PipelineVariant::NoRecovery;
        if (!std::isfinite(detail) || (recoveryMask & ~7u) != 0 || (lightMask & ~7u) != 0)
            return PipelineVariant::Generic;
        if ((recoveryMask & ~lightMask) == 0) return PipelineVariant::Light;
        return PipelineVariant::Generic;
    }
}
