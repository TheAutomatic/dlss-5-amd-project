#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace FSRDSignals
{
enum Type : int
{
    DirectDiffuse,
    DirectSpecular,
    IndirectDiffuse,
    IndirectSpecular
};
constexpr uint32_t Bit(int type) { return type >= 0 && type < 4 ? 1u << type : 0u; }
constexpr uint32_t All = 15u;
constexpr uint32_t DiffuseBits = Bit(DirectDiffuse) | Bit(IndirectDiffuse);
constexpr uint32_t SpecularBits = Bit(DirectSpecular) | Bit(IndirectSpecular);
constexpr const char* Names[] { "Direct Diffuse", "Direct Specular", "Indirect Diffuse", "Indirect Specular" };
constexpr int Count(uint32_t mask)
{
    return int((mask & 1u) != 0) + int((mask & 2u) != 0) + int((mask & 4u) != 0) + int((mask & 8u) != 0);
}
// No SignalCount/Signal1-4 layout keys were ever shipped in this product, so no
// slot-layout compat resolver exists here: routing is requested per lobe and the
// effective layout is derived by MakePlan on every validated frame.

// The title supplies one combined colour, so Direct/Indirect is not a light split: it only
// chooses which RR path denoises a lobe. Indirect reads a hit distance from the signal's
// alpha; Direct does not. Split halves the lobe across both paths.
enum Route : int
{
    Auto,
    Direct,
    Indirect,
    Split
};
constexpr const char* RouteCodes[] { "auto", "direct", "indirect", "split" };
constexpr int RouteFromCode(const std::string_view code)
{
    for (int route = 0; route < 4; ++route)
        if (code == RouteCodes[route])
            return route;
    return Auto;
}

// What the user asks for. The RR layout is derived from it on every validated frame, never
// stored, so the menu preview and the running context cannot disagree.
struct Request
{
    int diffuse = Auto;
    int specular = Auto;
    bool denoiseDiffuse = true;
    bool denoiseSpecular = true;
    bool albedoFix = false;  // Unsupported-albedo recovery
    bool estimate = false;   // Primary view depth stands in for a missing hit distance
    bool fixDiffuse = true;  // The fix also covers the diffuse share (a fourth RR signal)
};

enum Note : uint32_t
{
    DiffuseNeedsDistance = 1u,  // Indirect/Split diffuse without a diffuse hit distance
    SpecularNeedsDistance = 2u, // Indirect/Split specular without a specular hit distance
    FixNeedsDistance = 4u,      // the albedo fix keeps the main specular on Indirect
    FixNeedsBothLobes = 8u,     // the albedo fix needs diffuse and specular denoising
};

struct Plan
{
    uint32_t mask = 0;
    // Direct Specular carries the unmodulated copy of the main Indirect Specular.
    bool albedoFix = false;
    // Indirect Diffuse carries the unmodulated copy of the main Direct Diffuse (all four
    // signals). Without it the diffuse lobe keeps its own routing.
    bool diffuseCopy = false;
    uint32_t notes = 0;
    constexpr bool operator==(const Plan&) const = default;
};

// specularGuide/diffuseGuide: the title supplied that hit distance on THIS validated frame.
constexpr Plan MakePlan(Request request, bool specularGuide, bool diffuseGuide)
{
    Plan plan;
    if (!request.denoiseDiffuse && !request.denoiseSpecular)
        request.denoiseDiffuse = request.denoiseSpecular = true;
    const bool specularDistance = specularGuide || request.estimate;
    const bool diffuseDistance = diffuseGuide || request.estimate;
    const auto lobe = [&plan](int route, bool distance, int direct, int indirect, uint32_t note)
    {
        route = std::clamp(route, int(Auto), int(Split));
        if (route == Direct) return Bit(direct);
        if (!distance)
        {
            if (route != Auto) plan.notes |= note;
            return Bit(direct);
        }
        return route == Split ? Bit(direct) | Bit(indirect) : Bit(indirect);
    };
    // The specular copy is denoised as Direct Specular, so the main specular needs Indirect.
    // The diffuse copy reads its ray length from the title or primary view depth; replays
    // show the length does not change RR's result, so only the specular main needs a guide.
    if (request.albedoFix && request.denoiseDiffuse && request.denoiseSpecular && specularDistance)
    {
        plan.albedoFix = true;
        if (request.fixDiffuse)
        {
            plan.diffuseCopy = true;
            plan.mask = All;
            return plan;
        }
        plan.mask = SpecularBits;
    }
    else if (request.albedoFix)
        plan.notes |= !request.denoiseDiffuse || !request.denoiseSpecular ? FixNeedsBothLobes : FixNeedsDistance;
    if (request.denoiseDiffuse)
        plan.mask |= lobe(request.diffuse, diffuseDistance, DirectDiffuse, IndirectDiffuse, DiffuseNeedsDistance);
    if (request.denoiseSpecular && !plan.albedoFix)
        plan.mask |= lobe(request.specular, specularDistance, DirectSpecular, IndirectSpecular, SpecularNeedsDistance);
    return plan;
}

// The signals one frame can feed. An Indirect path reads this frame's hit distance (the
// title's, or primary view depth with estimate). The diffuse copy's Indirect Diffuse slot
// always has one: conversion gives it primary view depth where the title has none, and so
// does the lobe half that slot carries while the fix pauses.
constexpr uint32_t Feedable(const Plan& plan, bool estimate, bool specularGuide, bool diffuseGuide)
{
    return Bit(DirectDiffuse) | Bit(DirectSpecular) |
           (specularGuide || estimate ? Bit(IndirectSpecular) : 0u) |
           (diffuseGuide || estimate || plan.diffuseCopy ? Bit(IndirectDiffuse) : 0u);
}

// This is also the continuity payload for deferred feature replacement. Current
// availability selects the layout; ever-seen statistics must not resurrect a guide.
struct Guides
{
    bool specular = false;
    bool diffuse = false;
    bool seenSpecular = false;
    bool seenDiffuse = false;

    constexpr void Observe(bool currentSpecular, bool currentDiffuse)
    {
        specular = currentSpecular;
        diffuse = currentDiffuse;
        seenSpecular |= specular;
        seenDiffuse |= diffuse;
    }

    constexpr Plan Resolve(const Request& request) const { return MakePlan(request, specular, diffuse); }
    constexpr uint32_t FeedableMask(const Plan& plan, bool estimate) const
    { return Feedable(plan, estimate, specular, diffuse); }
};

// Maps this product's own retired Diffuse/SpecularSignalType keys (0 Direct,
// 1 Indirect, unset Auto) onto per-lobe routing. SignalCount/Signal1-4 were
// never shipped here and have no migration path.
constexpr Request FromSignalTypes(Request request, int diffuseType, int specularType)
{
    if (diffuseType >= 0) request.diffuse = diffuseType ? Indirect : Direct;
    if (specularType >= 0) request.specular = specularType ? Indirect : Direct;
    return request;
}

// Runtime -> menu report, packed into one atomic word.
struct Status
{
    bool observed = false;       // a frame passed validation
    bool specularGuide = false;  // title hit distances available on the current validated frame
    bool diffuseGuide = false;
    bool albedoFixActive = false; // the fix runs this frame (modulation and additive split allow it)
    Plan plan;

    constexpr uint32_t Pack() const
    {
        return (observed ? 1u : 0u) | (specularGuide ? 2u : 0u) | (diffuseGuide ? 4u : 0u) |
               (albedoFixActive ? 8u : 0u) | (plan.albedoFix ? 16u : 0u) | (plan.diffuseCopy ? 32u : 0u) |
               ((plan.mask & All) << 8) | ((plan.notes & 15u) << 12);
    }
    static constexpr Status Unpack(uint32_t word)
    {
        Status status;
        status.observed = word & 1u;
        status.specularGuide = word & 2u;
        status.diffuseGuide = word & 4u;
        status.albedoFixActive = word & 8u;
        status.plan.albedoFix = word & 16u;
        status.plan.diffuseCopy = word & 32u;
        status.plan.mask = (word >> 8) & All;
        status.plan.notes = (word >> 12) & 15u;
        return status;
    }
};

// Config adapters stay templates so this header compiles without Config.h in the tests.
template <class C> Request RequestFrom(const C& cfg)
{
    return { std::clamp(cfg.FfxDenoiserDiffuseRoute.value_or_default(), int(Auto), int(Split)),
             std::clamp(cfg.FfxDenoiserSpecularRoute.value_or_default(), int(Auto), int(Split)),
             cfg.FfxDenoiserDenoiseDiffuse.value_or_default(),
             cfg.FfxDenoiserDenoiseSpecular.value_or_default(),
             cfg.FfxDenoiserUnsupportedAlbedoRecovery.value_or_default(),
             cfg.FfxDenoiserEstimateHitDistances.value_or_default(),
             cfg.FfxDenoiserAlbedoBleedFixDiffuse.value_or_default() };
}
// The alternate replaces the full-strength specular reconstruction, so the fix runs only at
// the default 1/1 modulation and without the additive split.
template <class C> bool AlbedoFixAllowed(const C& cfg)
{
    return cfg.FfxDenoiserSpecularAlbedoDemodulation.value_or_default() == 1.0f &&
           cfg.FfxDenoiserDiffuseAlbedoModulation.value_or_default() == 1.0f &&
           cfg.FfxDenoiserAdditiveLightSplit.value_or_default() == 0.0f;
}
} // namespace FSRDSignals
