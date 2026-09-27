#pragma once
// ini / txt / env identifiers. Menu labels are UI-only and must never be used as ini keys.
// Config priority: Ins session / OptiScaler.ini > native-game-flags.txt / DLSS5_* env >
// compile defaults (see AGENTS.md and LmxxfNrRuntime ApplyFlagsFileFallback).
// daniel [DlssNrOnAmd] / dlssnr_on_amd.ini: Ins session > OptiScaler.ini (Save) >
// dlssnr_on_amd.ini / env > defaults. Prefer the same string on both sides.
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace CfgKey
{
inline constexpr const char *kSection = "DlssNr";
inline constexpr const char *kDanielSection = "DlssNrOnAmd";

// Cross-layer product keys use the upstream DLSS5_* names so ini == env == txt.
// Host _putenv writes that name; ApplyFlagsFileFallback will not override it.
inline constexpr const char *FitLarge = "DLSS5_FIT_LARGE";
inline constexpr const char *Pdl = "DLSS5_HIP_PDL";
// Pre-unification ini spellings. Read-only migration; never written back.
inline constexpr const char *FitLargeLegacy = "LmxxfFitLarge";
inline constexpr const char *PdlLegacy = "LmxxfPdl";
inline constexpr const char *AutoExposure = "LmxxfAutoExposure";
inline constexpr const char *AutoExposureScale = "LmxxfAutoExposureScale";
inline constexpr const char *PaperWhite = "LmxxfPaperWhite";
inline constexpr const char *AllowEnhancedBarriers = "LmxxfAllowEnhancedBarriers";
inline constexpr const char *EarlyExeWrap = "LmxxfEarlyExeWrap";
inline constexpr const char *Diagnostic = "LmxxfDiagnostic";

// Upstream-shared knobs (ini name == env name). Menu labels stay in DlssNr_Menu.cpp.
inline constexpr const char *NetworkHeight = "DLSS5_NETWORK_HEIGHT";
inline constexpr const char *WaveOwned = "DLSS5_HIP_WAVE_OWNED";
inline constexpr const char *C512M32 = "DLSS5_HIP_C512_M32";
inline constexpr const char *VitProjN64 = "DLSS5_HIP_VIT_PROJ_N64";
inline constexpr const char *SharedPool = "DLSS5_HIP_SHARED_POOL";
inline constexpr const char *MHByteStream = "DLSS5_HIP_MH_BYTE_STREAM";
inline constexpr const char *DecoderByte = "DLSS5_HIP_DECODER_BYTE";
inline constexpr const char *VitByteStream = "DLSS5_HIP_VIT_BYTE_STREAM";
inline constexpr const char *VitAdaptive = "DLSS5_VIT_ADAPTIVE";
inline constexpr const char *VitReusePeriod = "DLSS5_VIT_REUSE_PERIOD";
inline constexpr const char *VitReuseGlobal = "DLSS5_VIT_REUSE_GLOBAL";
inline constexpr const char *VitReuseLocal = "DLSS5_VIT_REUSE_LOCAL";
inline constexpr const char *VitReuseImage = "DLSS5_VIT_REUSE_IMAGE";
inline constexpr const char *VitReuseHotkey = "DLSS5_VIT_REUSE_HOTKEY";

// daniel [DlssNrOnAmd] channels. Same string in OptiScaler.ini when possible.
inline constexpr const char *ToneCurve = "ToneCurve";
inline constexpr const char *ToneLift = "ToneLift";
inline constexpr const char *Quality = "Quality";
inline constexpr const char *QueuePriority = "QueuePriority";
inline constexpr const char *QueuePriorityLegacy = "AmdQueuePriority";
// AmdInline=1 means daniel Async=0 (same-frame). Write Async on Save.
inline constexpr const char *Inline = "AmdInline";
inline constexpr const char *Async = "Async";

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
    ToneCurve,
    ToneLift,
    Quality,
    QueuePriority,
    QueuePriorityLegacy,
    Inline,
    Async,
    FitLarge,
    Pdl,
    FitLargeLegacy,
    PdlLegacy,
    AutoExposure,
    AutoExposureScale,
    PaperWhite,
    AllowEnhancedBarriers,
    EarlyExeWrap,
    Diagnostic,
    NetworkHeight,
    WaveOwned,
    C512M32,
    VitProjN64,
    SharedPool,
    MHByteStream,
    DecoderByte,
    VitByteStream,
    VitAdaptive,
    VitReusePeriod,
    VitReuseGlobal,
    VitReuseLocal,
    VitReuseImage,
    VitReuseHotkey,
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

// DLSS5_* env name for a product key. Unified keys are already DLSS5_*.
inline const char *EnvAlias(const char *iniKey)
{
    if (!iniKey)
        return nullptr;
    if (std::strncmp(iniKey, "DLSS5_", 6) == 0)
        return iniKey;
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

inline void PutEnvString(const char *iniKey, const char *value)
{
    const char *env = EnvAlias(iniKey);
    if (!env || !value)
        return;
    char entry[192];
    std::snprintf(entry, sizeof entry, "%s=%s", env, value);
    _putenv(entry);
}
} // namespace CfgKey
