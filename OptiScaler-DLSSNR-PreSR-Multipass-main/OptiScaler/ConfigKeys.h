#pragma once
// ini / txt / env identifiers. Menu labels are UI-only and must never be used as ini keys.
// Config priority: Ins session / OptiScaler.ini > native-game-flags.txt / DLSS5_* env >
// compile defaults (see AGENTS.md and LmxxfNrRuntime ApplyFlagsFileFallback).
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace CfgKey
{
inline constexpr const char *kSection = "DlssNr";

// Product keys that also have a DLSS5_* env alias. Host _putenv uses EnvAlias; txt must
// not override an env entry the host already wrote.
inline constexpr const char *FitLarge = "LmxxfFitLarge";
inline constexpr const char *Pdl = "LmxxfPdl";
inline constexpr const char *AutoExposure = "LmxxfAutoExposure";
inline constexpr const char *AutoExposureScale = "LmxxfAutoExposureScale";
inline constexpr const char *PaperWhite = "LmxxfPaperWhite";
inline constexpr const char *AllowEnhancedBarriers = "LmxxfAllowEnhancedBarriers";
inline constexpr const char *EarlyExeWrap = "LmxxfEarlyExeWrap";
inline constexpr const char *Diagnostic = "LmxxfDiagnostic";

// Known DlssNr ini keys (save path). Adding a menu control requires adding its key here
// first; labels stay in DlssNr_Menu.cpp only.
inline constexpr const char *const kKnown[] = {
    "Enabled",
    "RunBeforeSR",
    "ApplyAfterRR",
    "RRPasses",
    "RRWorkingScale",
    "ToggleKey",
    "TransferStrength",
    "ColourStrength",
    "MaxRatio",
    "Transfer",
    "ProbeD3D11",
    "WhitePointFromExposure",
    "DebugView",
    "Compare",
    "CompareSplit",
    "CompareZoom",
    "CompareSwap",
    "CompareTags",
    "TagScale",
    "WorkingScale",
    "ScalingDownscaler",
    "AutoCapture",
    "WhitePointSource",
    "WhitePointTrim",
    "ScanTrim",
    "ScanAnchorValue",
    "ScanAnchorWhitePoint",
    "ScanAnchors",
    "ScanInverted",
    "ScanMeter",
    "Passes",
    "UseProxy",
    "ProxyProbe",
    "ScanExposure",
    "WhitePointScale",
    "Preset",
    "Intensity",
    "Style",
    "Pass2Preset",
    "Pass2Style",
    "Pass3Preset",
    "Pass3Style",
    "LocalStructure",
    "LocalTone",
    "AmdNeuralLighting",
    "AmdNeuralLightingStrength",
    "AmdEncoding",
    "AmdSlots",
    "AmdModelScale",
    "AmdEveryFrame",
    "AmdSpinDraw",
    "AmdGraphicsWait",
    "NrBackend",
    "SkinStructure",
    "AutoMask",
    "SkinProtection",
    "SkinToneEnabled",
    "SkinDetail",
    "SkinTone",
    "ShowSkinMask",
    "Pass2AutoMask",
    "Pass3AutoMask",
    "ApplyModel",
    "HoldFrame",
    FitLarge,
    Pdl,
    AutoExposure,
    AutoExposureScale,
    PaperWhite,
    AllowEnhancedBarriers,
    EarlyExeWrap,
    Diagnostic,
};

inline bool IsKnown(const char *key)
{
    if (!key || !*key)
        return false;
    for (const char *k : kKnown)
        if (std::strcmp(k, key) == 0)
            return true;
    return false;
}

// DLSS5_* name for host putenv when the product key must win over flags/txt.
inline const char *EnvAlias(const char *iniKey)
{
    if (!iniKey)
        return nullptr;
    if (std::strcmp(iniKey, FitLarge) == 0)
        return "DLSS5_FIT_LARGE";
    if (std::strcmp(iniKey, Pdl) == 0)
        return "DLSS5_HIP_PDL";
    return nullptr;
}

// Host writes env so ApplyFlagsFileFallback cannot override an ini/menu choice.
inline void PutEnvAlias(const char *iniKey, bool on)
{
    const char *env = EnvAlias(iniKey);
    if (!env)
        return;
    char entry[160];
    std::snprintf(entry, sizeof entry, "%s=%d", env, on ? 1 : 0);
    _putenv(entry);
}
} // namespace CfgKey
