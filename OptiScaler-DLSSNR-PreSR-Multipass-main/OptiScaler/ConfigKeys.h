#pragma once
// ini / txt / env identifiers. Menu labels are UI-only and must never be used as ini keys.
// Config priority: Ins session / OptiScaler.ini > native-game-flags.txt / DLSS5_* env >
// compile defaults (see AGENTS.md and LmxxfNrRuntime ApplyFlagsFileFallback).
// daniel: Ins session > OptiScaler.ini [DlssNr] (Save) > dlssnr_on_amd.ini [DlssNrOnAmd] > defaults.
// OptiScaler.ini always uses kSection. kDanielSection is ONLY the daniel runtime file we
// write on Save Settings. Prefer the same key string on both sides (AmdInline is the
// exception: host 1 = same-frame, daniel Async=0).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <windows.h>

namespace CfgKey
{
inline constexpr const char *kMenuSection = "Menu";
inline constexpr const char *MenuWindowWidth = "WindowWidth";
inline constexpr const char *MenuWindowHeight = "WindowHeight";
inline constexpr const char *MenuWindowAnchor = "WindowAnchor";
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
inline constexpr const char *NrTimingEnabled = "NrTimingEnabled";
inline constexpr const char *NrTimingLog = "NrTimingLog";
inline constexpr const char *NrStabilizerEnabled = "NrStabilizerEnabled";
inline constexpr const char *NrStabilizerAlpha = "NrStabilizerAlpha";
inline constexpr const char *NrStabilizerThreshold = "NrStabilizerThreshold";
inline constexpr const char *NrOverallIntensity = "NrOverallIntensity";
inline constexpr const char *MochizukiIntensity = "MochizukiIntensity";
inline constexpr const char *MochizukiStyle = "MochizukiStyle";
inline constexpr const char *MochizukiLocalTone = "MochizukiLocalTone";
inline constexpr const char *MochizukiLocalStructure = "MochizukiLocalStructure";
inline constexpr const char *MochizukiSkinStructure = "MochizukiSkinStructure";
inline constexpr const char *MochizukiAutomaticMask = "MochizukiAutomaticMask";
inline constexpr const char *MochizukiDetailStrength = "MochizukiDetailStrength";
inline constexpr const char *MochizukiColourStrength = "MochizukiColourStrength";
inline constexpr const char *MochizukiMaxRatio = "MochizukiMaxRatio";
inline constexpr const char *MochizukiWhitePoint = "MochizukiWhitePoint";
inline constexpr const char *MochizukiModelScale = "MochizukiModelScale";
inline constexpr const char *MochizukiPasses = "MochizukiPasses";
inline constexpr const char *MochizukiTemporal = "MochizukiTemporal";
inline constexpr const char *MochizukiHistoryStrength = "MochizukiHistoryStrength";
inline constexpr const char *MochizukiPreprocess = "MochizukiPreprocess";
inline constexpr const char *MochizukiPreprocessExposure = "MochizukiPreprocessExposure";
inline constexpr const char *MochizukiPreprocessBiasEv = "MochizukiPreprocessBiasEv";
inline constexpr const char *MochizukiPreprocessCurve = "MochizukiPreprocessCurve";
inline constexpr const char *MochizukiPreprocessContrast = "MochizukiPreprocessContrast";
inline constexpr const char *MochizukiPreprocessSaturation = "MochizukiPreprocessSaturation";
inline constexpr const char *MochizukiApplyModel = "MochizukiApplyModel";
inline constexpr const char *MochizukiLinearInput = "MochizukiLinearInput";
inline constexpr const char *MochizukiMaxPasses = "MochizukiMaxPasses";
inline constexpr const char *MochizukiDynamicResolution = "MochizukiDynamicResolution";
inline constexpr const char *MochizukiPass2Override = "MochizukiPass2Override";
inline constexpr const char *MochizukiPass2Style = "MochizukiPass2Style";
inline constexpr const char *MochizukiPass2Intensity = "MochizukiPass2Intensity";
inline constexpr const char *MochizukiPass2LocalTone = "MochizukiPass2LocalTone";
inline constexpr const char *MochizukiPass2LocalStructure = "MochizukiPass2LocalStructure";
inline constexpr const char *MochizukiPass2SkinStructure = "MochizukiPass2SkinStructure";
inline constexpr const char *MochizukiPass2AutomaticMask = "MochizukiPass2AutomaticMask";
inline constexpr const char *MochizukiPass3Override = "MochizukiPass3Override";
inline constexpr const char *MochizukiPass3Style = "MochizukiPass3Style";
inline constexpr const char *MochizukiPass3Intensity = "MochizukiPass3Intensity";
inline constexpr const char *MochizukiPass3LocalTone = "MochizukiPass3LocalTone";
inline constexpr const char *MochizukiPass3LocalStructure = "MochizukiPass3LocalStructure";
inline constexpr const char *MochizukiPass3SkinStructure = "MochizukiPass3SkinStructure";
inline constexpr const char *MochizukiPass3AutomaticMask = "MochizukiPass3AutomaticMask";


// Upstream-shared knobs (ini name == env name). Menu labels stay in DlssNr_Menu.cpp.
inline constexpr const char *NetworkHeight = "DLSS5_NETWORK_HEIGHT";
inline constexpr const char *Network1080Rows = "DLSS5_NETWORK_1080_ROWS";
inline constexpr const char *LmxxfStyle = "DLSS5_STYLE";
inline constexpr const char *FormatFallback = "DLSS5_FORMAT_FALLBACK";
inline constexpr const char *SkipBlocks = "DLSS5_SKIP_BLOCKS";
inline constexpr char kDefaultSkipBlocks[] = "none";
inline constexpr const char *NetworkFreeRes = "DLSS5_NETWORK_FREE_RES";
inline constexpr const char *FastNumeric = "DLSS5_FAST_NUMERIC";
inline constexpr const char *MultiPass = "DLSS5_MULTI_PASS";
inline constexpr const char *MultiPassSkipBlocks = "DLSS5_MULTI_PASS_SKIP_BLOCKS";
inline constexpr const char *WaveOwned = "DLSS5_HIP_WAVE_OWNED";
inline constexpr const char *SwinRun = "DLSS5_HIP_SWIN_RUN";
inline constexpr const char *C512M32 = "DLSS5_HIP_C512_M32";
inline constexpr const char *VitProjN64 = "DLSS5_HIP_VIT_PROJ_N64";
inline constexpr const char *SharedPool = "DLSS5_HIP_SHARED_POOL";
inline constexpr const char *MHByteStream = "DLSS5_HIP_MH_BYTE_STREAM";
inline constexpr const char *DecoderByte = "DLSS5_HIP_DECODER_BYTE";
inline constexpr const char *VitByteStream = "DLSS5_HIP_VIT_BYTE_STREAM";
inline constexpr const char *VitStream = "DLSS5_HIP_VIT_STREAM";
inline constexpr const char *VitAdaptive = "DLSS5_VIT_ADAPTIVE";
inline constexpr const char *VitReusePeriod = "DLSS5_VIT_REUSE_PERIOD";
inline constexpr const char *VitReuseGlobal = "DLSS5_VIT_REUSE_GLOBAL";
inline constexpr const char *VitReuseLocal = "DLSS5_VIT_REUSE_LOCAL";
inline constexpr const char *VitReuseImage = "DLSS5_VIT_REUSE_IMAGE";
inline constexpr const char *VitReuseHotkey = "DLSS5_VIT_REUSE_HOTKEY";

// daniel [DlssNrOnAmd] channels. Same string in OptiScaler.ini when possible.
inline constexpr const char *ToneCurve = "ToneCurve";
inline constexpr const char *ToneLift = "ToneLift";
inline constexpr const char *AmdUseGameExposure = "AmdUseGameExposure";
inline constexpr const char *AmdToneChannels = "AmdToneChannels";
inline constexpr const char *Quality = "Quality";
inline constexpr const char *QueuePriority = "QueuePriority";
inline constexpr const char *QueuePriorityLegacy = "AmdQueuePriority";
// AmdInline=1 means daniel Async=0 (same-frame). Write Async on Save.
inline constexpr const char *Inline = "AmdInline";
inline constexpr const char *Async = "Async";

// Known product ini keys; [Menu] keys are explicitly identified below. Adding a control
// requires registering its key here first; display labels remain UI-only.
inline constexpr const char *const kKnown[] = {
    MenuWindowWidth, MenuWindowHeight, MenuWindowAnchor, // [Menu], no runtime env aliases
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
    "NrConvenience",
    NrTimingEnabled,
    NrTimingLog,
    NrStabilizerEnabled,
    NrStabilizerAlpha,
    NrStabilizerThreshold,
    NrOverallIntensity,
    MochizukiIntensity,
    MochizukiStyle,
    MochizukiLocalTone,
    MochizukiLocalStructure,
    MochizukiSkinStructure,
    MochizukiAutomaticMask,
    MochizukiDetailStrength,
    MochizukiColourStrength,
    MochizukiMaxRatio,
    MochizukiWhitePoint,
    MochizukiModelScale,
    MochizukiPasses,
    MochizukiTemporal,
    MochizukiHistoryStrength,
    MochizukiPreprocess,
    MochizukiPreprocessExposure,
    MochizukiPreprocessBiasEv,
    MochizukiPreprocessCurve,
    MochizukiPreprocessContrast,
    MochizukiPreprocessSaturation,
    MochizukiApplyModel,
    MochizukiLinearInput,
    MochizukiMaxPasses,
    MochizukiDynamicResolution,
    MochizukiPass2Override,
    MochizukiPass2Style,
    MochizukiPass2Intensity,
    MochizukiPass2LocalTone,
    MochizukiPass2LocalStructure,
    MochizukiPass2SkinStructure,
    MochizukiPass2AutomaticMask,
    MochizukiPass3Override,
    MochizukiPass3Style,
    MochizukiPass3Intensity,
    MochizukiPass3LocalTone,
    MochizukiPass3LocalStructure,
    MochizukiPass3SkinStructure,
    MochizukiPass3AutomaticMask,

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
    AmdUseGameExposure,
    AmdToneChannels,
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
    NetworkFreeRes,
    FastNumeric,
    MultiPass,
    MultiPassSkipBlocks,
    Network1080Rows,
    LmxxfStyle,
    FormatFallback,
    SkipBlocks,
    WaveOwned,
    SwinRun,
    C512M32,
    VitProjN64,
    SharedPool,
    MHByteStream,
    DecoderByte,
    VitByteStream,
    VitStream,
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

// Canonical CSV for the residual blocks accepted by upstream ParseSkipBlocks.
// Use a nonempty 'none' sentinel: an empty CRT environment value removes the key.
inline bool NormalizeSkipBlocks(const std::string &input, std::string &result)
{
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return false;
    const auto text = input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1);
    if (_stricmp(text.c_str(), "auto") == 0 || _stricmp(text.c_str(), "none") == 0)
    {
        result = _stricmp(text.c_str(), "auto") == 0 ? kDefaultSkipBlocks : "none";
        return true;
    }
    bool blocks[70] {};
    size_t pos = 0;
    while (pos < text.size())
    {
        const auto end = text.find(',', pos);
        const auto word = text.substr(pos, end == std::string::npos ? end : end - pos);
        const auto begin = word.find_first_not_of(" \t\r\n");
        const auto last = word.find_last_not_of(" \t\r\n");
        if (begin == std::string::npos)
            return false;
        unsigned block = 0;
        for (size_t i = begin; i <= last; ++i)
        {
            if (word[i] < '0' || word[i] > '9')
                return false;
            block = block * 10 + unsigned(word[i] - '0');
            if (block > 69)
                return false;
        }
        if (block == 0 || block == 39)
            return false;
        blocks[block] = true;
        if (end == std::string::npos)
            break;
        pos = end + 1;
        if (pos == text.size())
            return false;
    }
    result.clear();
    for (unsigned block = 1; block < 70; ++block)
        if (blocks[block])
        {
            if (!result.empty())
                result += ',';
            result += std::to_string(block);
        }
    return true;
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
// The host and runtime can have separate CRT environments, including two MSVC
// static CRTs. Publish to Win32; the runtime imports these keys before using them.
inline void PutEnvAlias(const char *iniKey, bool on)
{
    const char *env = EnvAlias(iniKey);
    if (!env)
        return;
    char entry[160];
    std::snprintf(entry, sizeof entry, "%s=%d", env, on ? 1 : 0);
    _putenv(entry);
    SetEnvironmentVariableA(env, on ? "1" : "0");
}

inline void PutEnvString(const char *iniKey, const char *value)
{
    const char *env = EnvAlias(iniKey);
    if (!env || !value)
        return;
    const std::string entry = std::string(env) + "=" + value;
    _putenv(entry.c_str());
    SetEnvironmentVariableA(env, value);
}

// Called inside the runtime's CRT. Updating the process environment alone does not
// refresh its getenv cache. Missing entries leave runtime/flags fallbacks intact.
inline void SyncEnvAliasesFromProcess()
{
    for (const char *key : kKnown)
    {
        const char *env = EnvAlias(key);
        if (!env)
            continue;
        char value[256] {};
        const DWORD n = GetEnvironmentVariableA(env, value, sizeof value);
        if (!n || n >= sizeof value)
            continue;
        const char *current = std::getenv(env);
        if (!current || std::strcmp(current, value) != 0)
        {
            const std::string entry = std::string(env) + "=" + value;
            _putenv(entry.c_str());
        }
    }
}
} // namespace CfgKey
