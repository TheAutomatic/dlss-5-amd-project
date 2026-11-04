#pragma once
#include <algorithm>
#include <optional>
#include <string>
#include <type_traits>
#include "../../ConfigKeys.h"
#include "FSRDSignalPolicy.h"

// FSR-RR Floor/recovery profile policy. Pure production logic, templated on the
// Config-like holder so the CPU tests exercise the same code the product runs
// (Config::ApplyFfxDenoiserProfile/GetFfxDenoiserProfile delegate here).
//
// Profiles own the Floor/recovery policy; signal routing and the albedo fix are
// per-game choices outside the profile. Manual edits persist as Custom, and an
// explicit per-key Floor-policy value in the INI wins over the profile on load
// (the profile degrades to Custom instead of overwriting the preference).
namespace FSRDProfiles
{
enum Profile : int
{
    Custom = -1,
    Fast = 0,
    Balanced = 1,
    Quality = 2
};
constexpr const char* Names[] { "Custom", "Fast", "Balanced", "Quality" };
constexpr const char* Code(const int profile)
{
    return profile >= Fast && profile <= Quality ? Names[profile + 1] : Names[0];
}

// Applies the profile's Floor/recovery policy and returns the profile clamped to
// a real selection. The anchor/modulation/additive keys return to Auto: a profile
// change re-derives them, and saved manual values already flipped the stored
// profile to Custom so they never reach this path.
template <class C> int Apply(C& cfg, int profile)
{
    profile = std::clamp(profile, int(Fast), int(Quality));
    cfg.FfxDenoiserProfile = profile;
    cfg.FfxDenoiserFloorEnabled = profile != Fast;
    cfg.FfxDenoiserFloorFastMode = profile == Balanced;
    cfg.FfxDenoiserFloorRecovery = profile == Fast ? 0.0f : 1.0f;
    cfg.FfxDenoiserFloorFlatRecovery = profile != Fast;
    cfg.FfxDenoiserFloorSpecularRecovery = profile != Fast;
    cfg.FfxDenoiserFloorDiffuseRecovery = profile == Quality;
    cfg.FfxDenoiserFloorFlatNoiseMethod = 0;
    cfg.FfxDenoiserFloorSpecularNoiseMethod = 1;
    cfg.FfxDenoiserFloorDiffuseNoiseMethod = 1;
    cfg.FfxDenoiserFloorHandoverAnchorClamp.reset();
    cfg.FfxDenoiserFloorHandoverCorrelationMix.reset();
    cfg.FfxDenoiserFloorLumaRecovery.reset();
    cfg.FfxDenoiserFloorChromaRecovery.reset();
    cfg.FfxDenoiserSpecularAlbedoDemodulation.reset();
    cfg.FfxDenoiserDiffuseAlbedoModulation.reset();
    cfg.FfxDenoiserAdditiveLightSplit.reset();
    return profile;
}

// Load-time application: fill every unspecified profile-owned key without erasing
// explicit false, zero, or manual modulation/anchor preferences. Interactive Apply
// remains intentionally destructive; Detect reports Custom for mixed policies.
template <class C> void FillUnspecified(C& cfg, int profile)
{
    profile = std::clamp(profile, int(Fast), int(Quality));
    if (!cfg.FfxDenoiserFloorEnabled.has_value()) cfg.FfxDenoiserFloorEnabled = profile != Fast;
    if (!cfg.FfxDenoiserFloorFastMode.has_value()) cfg.FfxDenoiserFloorFastMode = profile == Balanced;
    if (!cfg.FfxDenoiserFloorRecovery.has_value()) cfg.FfxDenoiserFloorRecovery = profile == Fast ? 0.0f : 1.0f;
    if (!cfg.FfxDenoiserFloorFlatRecovery.has_value()) cfg.FfxDenoiserFloorFlatRecovery = profile != Fast;
    if (!cfg.FfxDenoiserFloorSpecularRecovery.has_value()) cfg.FfxDenoiserFloorSpecularRecovery = profile != Fast;
    if (!cfg.FfxDenoiserFloorDiffuseRecovery.has_value()) cfg.FfxDenoiserFloorDiffuseRecovery = profile == Quality;
    if (!cfg.FfxDenoiserFloorFlatNoiseMethod.has_value()) cfg.FfxDenoiserFloorFlatNoiseMethod = 0;
    if (!cfg.FfxDenoiserFloorSpecularNoiseMethod.has_value()) cfg.FfxDenoiserFloorSpecularNoiseMethod = 1;
    if (!cfg.FfxDenoiserFloorDiffuseNoiseMethod.has_value()) cfg.FfxDenoiserFloorDiffuseNoiseMethod = 1;
    if (!cfg.FfxDenoiserFloorHandoverAnchorClamp.has_value()) cfg.FfxDenoiserFloorHandoverAnchorClamp = 4.0f;
    if (!cfg.FfxDenoiserFloorHandoverCorrelationMix.has_value()) cfg.FfxDenoiserFloorHandoverCorrelationMix = 1.0f;
    if (!cfg.FfxDenoiserFloorLumaRecovery.has_value()) cfg.FfxDenoiserFloorLumaRecovery = 1.0f;
    if (!cfg.FfxDenoiserFloorChromaRecovery.has_value()) cfg.FfxDenoiserFloorChromaRecovery = 1.0f;
    if (!cfg.FfxDenoiserSpecularAlbedoDemodulation.has_value()) cfg.FfxDenoiserSpecularAlbedoDemodulation = 1.0f;
    if (!cfg.FfxDenoiserDiffuseAlbedoModulation.has_value()) cfg.FfxDenoiserDiffuseAlbedoModulation = 1.0f;
    if (!cfg.FfxDenoiserAdditiveLightSplit.has_value()) cfg.FfxDenoiserAdditiveLightSplit = 0.0f;
}

// The profile the current values describe: any departure from a profile's Floor
// policy reads as Custom, so saving keeps the manual values.
template <class C> int Detect(const C& cfg)
{
    for (int profile = Fast; profile <= Quality; ++profile)
    {
        if (cfg.FfxDenoiserFloorEnabled.value_or_default() != (profile != Fast) ||
            cfg.FfxDenoiserFloorFastMode.value_or_default() != (profile == Balanced) ||
            cfg.FfxDenoiserFloorRecovery.value_or_default() != (profile == Fast ? 0.0f : 1.0f) ||
            cfg.FfxDenoiserFloorFlatRecovery.value_or_default() != (profile != Fast) ||
            cfg.FfxDenoiserFloorSpecularRecovery.value_or_default() != (profile != Fast) ||
            cfg.FfxDenoiserFloorDiffuseRecovery.value_or_default() != (profile == Quality) ||
            cfg.FfxDenoiserFloorFlatNoiseMethod.value_or_default() != 0 ||
            cfg.FfxDenoiserFloorSpecularNoiseMethod.value_or_default() != 1 ||
            cfg.FfxDenoiserFloorDiffuseNoiseMethod.value_or_default() != 1 ||
            cfg.FfxDenoiserSpecularAlbedoDemodulation.value_or_default() != 1.0f ||
            cfg.FfxDenoiserDiffuseAlbedoModulation.value_or_default() != 1.0f ||
            cfg.FfxDenoiserAdditiveLightSplit.value_or_default() != 0.0f)
            continue;
        // Save manual anchor changes as Custom, so reloading keeps them.
        if (cfg.FfxDenoiserFloorHandoverAnchorClamp.value_or_default() != 4.0f ||
            cfg.FfxDenoiserFloorHandoverCorrelationMix.value_or_default() != 1.0f ||
            cfg.FfxDenoiserFloorLumaRecovery.value_or_default() != 1.0f ||
            cfg.FfxDenoiserFloorChromaRecovery.value_or_default() != 1.0f)
            continue;
        return profile;
    }
    return Custom;
}

// True when an explicit per-key Floor-policy value disagrees with what the
// requested profile would set. The loader degrades to Custom in that case: a
// user's explicit preference is never overwritten by the profile on load.
template <class C> bool HasExplicitPolicyOverride(const C& cfg, int profile)
{
    profile = std::clamp(profile, int(Fast), int(Quality));
    if (cfg.FfxDenoiserFloorEnabled.has_value() &&
        cfg.FfxDenoiserFloorEnabled.value() != (profile != Fast))
        return true;
    if (cfg.FfxDenoiserFloorFastMode.has_value() &&
        cfg.FfxDenoiserFloorFastMode.value() != (profile == Balanced))
        return true;
    if (cfg.FfxDenoiserFloorRecovery.has_value() &&
        cfg.FfxDenoiserFloorRecovery.value() != (profile == Fast ? 0.0f : 1.0f))
        return true;
    if (cfg.FfxDenoiserFloorFlatRecovery.has_value() &&
        cfg.FfxDenoiserFloorFlatRecovery.value() != (profile != Fast))
        return true;
    if (cfg.FfxDenoiserFloorSpecularRecovery.has_value() &&
        cfg.FfxDenoiserFloorSpecularRecovery.value() != (profile != Fast))
        return true;
    if (cfg.FfxDenoiserFloorDiffuseRecovery.has_value() &&
        cfg.FfxDenoiserFloorDiffuseRecovery.value() != (profile == Quality))
        return true;
    if (cfg.FfxDenoiserFloorFlatNoiseMethod.has_value() &&
        cfg.FfxDenoiserFloorFlatNoiseMethod.value() != 0)
        return true;
    if (cfg.FfxDenoiserFloorSpecularNoiseMethod.has_value() &&
        cfg.FfxDenoiserFloorSpecularNoiseMethod.value() != 1)
        return true;
    if (cfg.FfxDenoiserFloorDiffuseNoiseMethod.has_value() &&
        cfg.FfxDenoiserFloorDiffuseNoiseMethod.value() != 1)
        return true;
    return false;
}

// True when an explicit anchor/modulation/additive value disagrees with the
// Auto defaults a profile would reset to. The loader degrades to Custom here
// too: Apply() resets these keys, so an explicit non-default preference must
// not reach that path under a selected profile.
template <class C> bool HasExplicitAnchorOverride(const C& cfg)
{
    if (cfg.FfxDenoiserFloorHandoverAnchorClamp.has_value() &&
        cfg.FfxDenoiserFloorHandoverAnchorClamp.value() != 4.0f)
        return true;
    if (cfg.FfxDenoiserFloorHandoverCorrelationMix.has_value() &&
        cfg.FfxDenoiserFloorHandoverCorrelationMix.value() != 1.0f)
        return true;
    if (cfg.FfxDenoiserFloorLumaRecovery.has_value() &&
        cfg.FfxDenoiserFloorLumaRecovery.value() != 1.0f)
        return true;
    if (cfg.FfxDenoiserFloorChromaRecovery.has_value() &&
        cfg.FfxDenoiserFloorChromaRecovery.value() != 1.0f)
        return true;
    if (cfg.FfxDenoiserSpecularAlbedoDemodulation.has_value() &&
        cfg.FfxDenoiserSpecularAlbedoDemodulation.value() != 1.0f)
        return true;
    if (cfg.FfxDenoiserDiffuseAlbedoModulation.has_value() &&
        cfg.FfxDenoiserDiffuseAlbedoModulation.value() != 1.0f)
        return true;
    if (cfg.FfxDenoiserAdditiveLightSplit.has_value() &&
        cfg.FfxDenoiserAdditiveLightSplit.value() != 0.0f)
        return true;
    return false;
}

// Reader(key, std::type_identity<T>{}) returns optional<T>; Writer accepts that
// same typed optional. Config's INI adapters use this exact load/save path.
template <class C, class Reader> void Load(C& cfg, Reader read)
{
    cfg.FsrRrEnabled.set_from_config(read(CfgKey::RrEnabled, std::type_identity<bool>{}));
    cfg.FfxDenoiserProfile.set_from_config(read(CfgKey::RrProfile, std::type_identity<int>{}));
    cfg.FfxDenoiserUseAmdDefaults.set_from_config(read(CfgKey::RrUseAmdDefaults, std::type_identity<bool>{}));
    cfg.FfxDenoiserDisocThreshold.set_from_config(read(CfgKey::RrDisocclusionThreshold, std::type_identity<float>{}));
    cfg.FfxDenoiserCrossBlNormStr.set_from_config(read(CfgKey::RrCrossBilateralNormalStrength, std::type_identity<float>{}));
    cfg.FfxDenoiserStabilityBias.set_from_config(read(CfgKey::RrTemporalStabilityBias, std::type_identity<float>{}));
    cfg.FfxDenoiserMaxRadiance.set_from_config(read(CfgKey::RrMaxRadiance, std::type_identity<float>{}));
    cfg.FfxDenoiserRadianceClip.set_from_config(read(CfgKey::RrRadianceClipDeviation, std::type_identity<float>{}));
    cfg.FfxDenoiserGaussKernRelax.set_from_config(read(CfgKey::RrGaussianKernelRelaxation, std::type_identity<float>{}));
    cfg.FfxDenoiserHardwareDepth.set_from_config(read(CfgKey::RrHardwareDepth, std::type_identity<bool>{}));
    cfg.FfxDenoiserIndex.set_from_config(read(CfgKey::RrProviderIndex, std::type_identity<int>{}));
    cfg.FfxDenoiserDebugDepthMax.set_from_config(read(CfgKey::RrDebugViewLinearDepthMax, std::type_identity<float>{}));
    cfg.FfxDenoiserDebugViewport.set_from_config(read(CfgKey::RrDebugViewViewport, std::type_identity<int>{}));
    cfg.FfxDenoiserInternalDebugViews.set_from_config(read(CfgKey::RrInternalDebugViews, std::type_identity<bool>{}));
    cfg.FfxDenoiserDiagnostics.set_from_config(read(CfgKey::RrDiagnostics, std::type_identity<bool>{}));
    cfg.FfxDenoiserDiffuseHitDistance.set_from_config(read(CfgKey::RrDiffuseHitDistance, std::type_identity<bool>{}));
    cfg.FfxDenoiserFloorEnabled.set_from_config(read(CfgKey::RrFloorEnabled, std::type_identity<bool>{}));
    cfg.FfxDenoiserFloorFastMode.set_from_config(read(CfgKey::RrFloorFastMode, std::type_identity<bool>{}));
    cfg.FfxDenoiserFloorRecovery.set_from_config(read(CfgKey::RrFloorRecovery, std::type_identity<float>{}));
    cfg.FfxDenoiserSpecularAlbedoDemodulation.set_from_config(read(CfgKey::RrSpecularAlbedoDemodulation, std::type_identity<float>{}));
    cfg.FfxDenoiserDiffuseAlbedoModulation.set_from_config(read(CfgKey::RrDiffuseAlbedoModulation, std::type_identity<float>{}));
    cfg.FfxDenoiserAdditiveLightSplit.set_from_config(read(CfgKey::RrAdditiveLightSplit, std::type_identity<float>{}));
    cfg.FfxDenoiserUnsupportedAlbedoRecovery.set_from_config(read(CfgKey::RrAlbedoBleedFix, std::type_identity<bool>{}));
    cfg.FfxDenoiserAlbedoBleedFixDiffuse.set_from_config(read(CfgKey::RrAlbedoBleedFixDiffuse, std::type_identity<bool>{}));
    cfg.FfxDenoiserFloorFlatRecovery.set_from_config(read(CfgKey::RrFloorFlatRecovery, std::type_identity<bool>{}));
    cfg.FfxDenoiserFloorSpecularRecovery.set_from_config(read(CfgKey::RrFloorSpecularRecovery, std::type_identity<bool>{}));
    cfg.FfxDenoiserFloorDiffuseRecovery.set_from_config(read(CfgKey::RrFloorDiffuseRecovery, std::type_identity<bool>{}));
    cfg.FfxDenoiserFloorFlatNoiseMethod.set_from_config(read(CfgKey::RrFloorFlatNoiseMethod, std::type_identity<int>{}));
    cfg.FfxDenoiserFloorSpecularNoiseMethod.set_from_config(read(CfgKey::RrFloorSpecularNoiseMethod, std::type_identity<int>{}));
    cfg.FfxDenoiserFloorDiffuseNoiseMethod.set_from_config(read(CfgKey::RrFloorDiffuseNoiseMethod, std::type_identity<int>{}));
    cfg.FfxDenoiserFloorLumaRecovery.set_from_config(read(CfgKey::RrFloorLumaRecovery, std::type_identity<float>{}));
    cfg.FfxDenoiserFloorChromaRecovery.set_from_config(read(CfgKey::RrFloorChromaRecovery, std::type_identity<float>{}));
    cfg.FfxDenoiserFloorHandoverCorrelationMix.set_from_config(read(CfgKey::RrFloorHandoverCorrelationMix, std::type_identity<float>{}));
    cfg.FfxDenoiserFloorHandoverAnchorClamp.set_from_config(read(CfgKey::RrFloorHandoverAnchorClamp, std::type_identity<float>{}));
    cfg.FfxDenoiserDemodDivisorFloor.set_from_config(read(CfgKey::RrDemodDivisorFloor, std::type_identity<float>{}));
    cfg.FfxDenoiserDenoiseDiffuse.set_from_config(read(CfgKey::RrDenoiseDiffuse, std::type_identity<bool>{}));
    cfg.FfxDenoiserDenoiseSpecular.set_from_config(read(CfgKey::RrDenoiseSpecular, std::type_identity<bool>{}));
    cfg.FfxDenoiserEstimateHitDistances.set_from_config(read(CfgKey::RrEstimateHitDistances, std::type_identity<bool>{}));
    cfg.FfxDenoiserGpuTimings.set_from_config(read(CfgKey::RrGpuTimings, std::type_identity<bool>{}));
    cfg.FfxDenoiserTaggedNormalRoughness.set_from_config(read(CfgKey::RrTaggedNormalRoughness, std::type_identity<bool>{}));
    cfg.FfxDenoiserTaggedAmbientOcclusion.set_from_config(read(CfgKey::RrTaggedAmbientOcclusion, std::type_identity<bool>{}));
    cfg.FfxDenoiserNormalsInViewSpace.set_from_config(read(CfgKey::RrNormalsInViewSpace, std::type_identity<bool>{}));
    cfg.FfxDenoiserUseTitleLinearDepth.set_from_config(read(CfgKey::RrUseTitleLinearDepth, std::type_identity<bool>{}));
    cfg.FfxDenoiserResponsivityThreshold.set_from_config(read(CfgKey::RrResponsivityTrustThreshold, std::type_identity<float>{}));
    cfg.FfxDenoiserResponsivityInvert.set_from_config(read(CfgKey::RrResponsivityInvert, std::type_identity<bool>{}));
    cfg.FfxDenoiserBiasMaskStrength.set_from_config(read(CfgKey::RrBiasMaskStrength, std::type_identity<float>{}));
    cfg.FfxDenoiserDebugMode.set_from_config(read(CfgKey::RrDebugMode, std::type_identity<uint64_t>{}));
    const auto readRoute = [&read](const char* key) -> std::optional<int> {
        const auto code = read(key, std::type_identity<std::string>{});
        if (!code) return std::nullopt;
        return FSRDSignals::RouteFromCode(*code);
    };
    cfg.FfxDenoiserDiffuseRoute.set_from_config(readRoute(CfgKey::RrDiffuseSignal));
    cfg.FfxDenoiserSpecularRoute.set_from_config(readRoute(CfgKey::RrSpecularSignal));
    const auto legacy = FSRDSignals::FromSignalTypes({},
        read("DiffuseSignalType", std::type_identity<int>{}).value_or(-1),
        read("SpecularSignalType", std::type_identity<int>{}).value_or(-1));
    if (!cfg.FfxDenoiserDiffuseRoute.has_value() && legacy.diffuse != FSRDSignals::Auto)
        cfg.FfxDenoiserDiffuseRoute.set_from_config(legacy.diffuse);
    if (!cfg.FfxDenoiserSpecularRoute.has_value() && legacy.specular != FSRDSignals::Auto)
        cfg.FfxDenoiserSpecularRoute.set_from_config(legacy.specular);
    if (cfg.FfxDenoiserProfile.value_or_default() >= 0)
        FillUnspecified(cfg, cfg.FfxDenoiserProfile.value_or_default());
}

template <class C, class Writer> void Save(C& cfg, Writer write)
{
    write(CfgKey::RrEnabled, cfg.FsrRrEnabled.value_for_config_ignore_default());
    write(CfgKey::RrProfile, std::optional<int>(Detect(cfg)));
    write(CfgKey::RrUseAmdDefaults, cfg.FfxDenoiserUseAmdDefaults.value_for_config_ignore_default());
    write(CfgKey::RrDisocclusionThreshold, cfg.FfxDenoiserDisocThreshold.value_for_config_ignore_default());
    write(CfgKey::RrCrossBilateralNormalStrength, cfg.FfxDenoiserCrossBlNormStr.value_for_config_ignore_default());
    write(CfgKey::RrTemporalStabilityBias, cfg.FfxDenoiserStabilityBias.value_for_config_ignore_default());
    write(CfgKey::RrMaxRadiance, cfg.FfxDenoiserMaxRadiance.value_for_config_ignore_default());
    write(CfgKey::RrRadianceClipDeviation, cfg.FfxDenoiserRadianceClip.value_for_config_ignore_default());
    write(CfgKey::RrGaussianKernelRelaxation, cfg.FfxDenoiserGaussKernRelax.value_for_config_ignore_default());
    write(CfgKey::RrHardwareDepth, cfg.FfxDenoiserHardwareDepth.value_for_config());
    write(CfgKey::RrProviderIndex, cfg.FfxDenoiserIndex.value_for_config_ignore_default());
    write(CfgKey::RrDebugViewLinearDepthMax, cfg.FfxDenoiserDebugDepthMax.value_for_config_ignore_default());
    write(CfgKey::RrDebugViewViewport, cfg.FfxDenoiserDebugViewport.value_for_config_ignore_default());
    write(CfgKey::RrInternalDebugViews, cfg.FfxDenoiserInternalDebugViews.value_for_config_ignore_default());
    write(CfgKey::RrDiagnostics, cfg.FfxDenoiserDiagnostics.value_for_config_ignore_default());
    write(CfgKey::RrDiffuseHitDistance, cfg.FfxDenoiserDiffuseHitDistance.value_for_config_ignore_default());
    write(CfgKey::RrFloorEnabled, cfg.FfxDenoiserFloorEnabled.value_for_config_ignore_default());
    write(CfgKey::RrFloorFastMode, cfg.FfxDenoiserFloorFastMode.value_for_config_ignore_default());
    write(CfgKey::RrFloorRecovery, cfg.FfxDenoiserFloorRecovery.value_for_config_ignore_default());
    write(CfgKey::RrSpecularAlbedoDemodulation, cfg.FfxDenoiserSpecularAlbedoDemodulation.value_for_config_ignore_default());
    write(CfgKey::RrDiffuseAlbedoModulation, cfg.FfxDenoiserDiffuseAlbedoModulation.value_for_config_ignore_default());
    write(CfgKey::RrAdditiveLightSplit, cfg.FfxDenoiserAdditiveLightSplit.value_for_config_ignore_default());
    write(CfgKey::RrAlbedoBleedFix, cfg.FfxDenoiserUnsupportedAlbedoRecovery.value_for_config_ignore_default());
    write(CfgKey::RrAlbedoBleedFixDiffuse, cfg.FfxDenoiserAlbedoBleedFixDiffuse.value_for_config_ignore_default());
    write(CfgKey::RrFloorFlatRecovery, cfg.FfxDenoiserFloorFlatRecovery.value_for_config_ignore_default());
    write(CfgKey::RrFloorSpecularRecovery, cfg.FfxDenoiserFloorSpecularRecovery.value_for_config_ignore_default());
    write(CfgKey::RrFloorDiffuseRecovery, cfg.FfxDenoiserFloorDiffuseRecovery.value_for_config_ignore_default());
    write(CfgKey::RrFloorFlatNoiseMethod, cfg.FfxDenoiserFloorFlatNoiseMethod.value_for_config_ignore_default());
    write(CfgKey::RrFloorSpecularNoiseMethod, cfg.FfxDenoiserFloorSpecularNoiseMethod.value_for_config_ignore_default());
    write(CfgKey::RrFloorDiffuseNoiseMethod, cfg.FfxDenoiserFloorDiffuseNoiseMethod.value_for_config_ignore_default());
    write(CfgKey::RrFloorLumaRecovery, cfg.FfxDenoiserFloorLumaRecovery.value_for_config_ignore_default());
    write(CfgKey::RrFloorChromaRecovery, cfg.FfxDenoiserFloorChromaRecovery.value_for_config_ignore_default());
    write(CfgKey::RrFloorHandoverCorrelationMix, cfg.FfxDenoiserFloorHandoverCorrelationMix.value_for_config_ignore_default());
    write(CfgKey::RrFloorHandoverAnchorClamp, cfg.FfxDenoiserFloorHandoverAnchorClamp.value_for_config_ignore_default());
    write(CfgKey::RrDemodDivisorFloor, cfg.FfxDenoiserDemodDivisorFloor.value_for_config_ignore_default());
    write(CfgKey::RrDenoiseDiffuse, cfg.FfxDenoiserDenoiseDiffuse.value_for_config_ignore_default());
    write(CfgKey::RrDenoiseSpecular, cfg.FfxDenoiserDenoiseSpecular.value_for_config_ignore_default());
    write(CfgKey::RrEstimateHitDistances, cfg.FfxDenoiserEstimateHitDistances.value_for_config_ignore_default());
    write(CfgKey::RrGpuTimings, cfg.FfxDenoiserGpuTimings.value_for_config_ignore_default());
    write(CfgKey::RrTaggedNormalRoughness, cfg.FfxDenoiserTaggedNormalRoughness.value_for_config_ignore_default());
    write(CfgKey::RrTaggedAmbientOcclusion, cfg.FfxDenoiserTaggedAmbientOcclusion.value_for_config_ignore_default());
    write(CfgKey::RrNormalsInViewSpace, cfg.FfxDenoiserNormalsInViewSpace.value_for_config_ignore_default());
    write(CfgKey::RrUseTitleLinearDepth, cfg.FfxDenoiserUseTitleLinearDepth.value_for_config_ignore_default());
    write(CfgKey::RrResponsivityTrustThreshold, cfg.FfxDenoiserResponsivityThreshold.value_for_config_ignore_default());
    write(CfgKey::RrResponsivityInvert, cfg.FfxDenoiserResponsivityInvert.value_for_config_ignore_default());
    write(CfgKey::RrBiasMaskStrength, cfg.FfxDenoiserBiasMaskStrength.value_for_config_ignore_default());
    write(CfgKey::RrDebugMode, cfg.FfxDenoiserDebugMode.value_for_config_ignore_default());
    write(CfgKey::RrDiffuseSignal, std::optional<std::string>(FSRDSignals::RouteCodes[std::clamp(cfg.FfxDenoiserDiffuseRoute.value_or_default(), 0, 3)]));
    write(CfgKey::RrSpecularSignal, std::optional<std::string>(FSRDSignals::RouteCodes[std::clamp(cfg.FfxDenoiserSpecularRoute.value_or_default(), 0, 3)]));
}
} // namespace FSRDProfiles
