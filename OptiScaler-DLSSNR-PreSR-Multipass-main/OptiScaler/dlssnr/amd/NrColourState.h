#pragma once
#include <d3d12.h>
#include <optional>

namespace DlssNr
{
// FSR-RR composition is a UAV texture returned in both shader-read states.
// Game colour/output hotfixes describe different resources and cannot own it.
inline constexpr auto kRrComposedColourState = static_cast<D3D12_RESOURCE_STATES>(
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

inline D3D12_RESOURCE_STATES ResolveNrColourState(bool beforeUpscale,
    std::optional<D3D12_RESOURCE_STATES> gameColour,
    std::optional<D3D12_RESOURCE_STATES> gameOutput,
    std::optional<D3D12_RESOURCE_STATES> ownedColour = {})
{
    if (ownedColour)
        return *ownedColour;
    if (!beforeUpscale)
        return gameOutput.value_or(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return gameColour.value_or(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
} // namespace DlssNr
