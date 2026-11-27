"""Built-in dlssnr-amd.ini presets (by Al菲比) for the Linux launcher.

Mirrors the Windows launcher's BuiltInAsiPresets.cs + MochizukiShareCode.Apply merge
semantics: preserves user hotkeys, language, non-style sections and comments, resets
conflicting style globals, writes [Layer1]/[Layer2] with Optimized-Ultra networks,
retires stale higher layers, and creates an atomic backup before replacing the INI.
"""
from __future__ import annotations

import datetime
import os
import re
import secrets
import tempfile
from pathlib import Path
from typing import Optional

_HEADER_RE = re.compile(r"^\s*\[([^\]]+)\]\s*(?:[;#].*)?$")
_ASSIGN_RE = re.compile(r"^(\s*)([^=;#]+?)(\s*=\s*)([^;#]*)(.*)$")

_RENODX_KEYS = {
    "NeuralUplift", "NRHookPoint", "NRIntensity", "NRStyle", "NRLocalTone", "NRLocalStructure",
    "NRSkinStructure", "NRAutoMask", "NRSkinIndependent", "NRPassesMode", "NRFastMethod",
    "NRFastExtrapolationPasses", "NRFastExtrapolationHaloLimit", "NRFastExtrapolationFine",
    "NRFastFeedbackPasses", "NRFastFeedbackHaloLimit", "NRFastFeedbackFine",
    "NRFastHybridPasses", "NRFastHybridHaloLimit", "NRFastHybridFine", "NRFastEditStrength",
    "NRFastInterleavedPasses", "NRFastInterleavedPreset", "NRFastInterleavedDepthDisocclusion",
    "NRPasses", "NRUnlockPasses", "NRChainedHistory", "NRNoiseEveryFrame", "NRPreHistory", "NRPostHistory",
    "NRPass2Transfer", "NRPass2Color", "NRPass2Intensity", "NRPass2FollowPass1", "NRPass2Style",
    "NRPass2LocalTone", "NRPass2Structure", "NRPass2Skin", "NRPass2AutoMask",
    "NRPass3Transfer", "NRPass3Color", "NRPass3Intensity", "NRPass3FollowPass1", "NRPass3Style",
    "NRPass3LocalTone", "NRPass3Structure", "NRPass3Skin", "NRPass3AutoMask",
    "NRPass4Transfer", "NRPass4Color", "NRPass4Intensity", "NRPass4FollowPass1", "NRPass4Style",
    "NRPass4LocalTone", "NRPass4Structure", "NRPass4Skin", "NRPass4AutoMask",
    "NRColorStrength", "NRTransferStrength", "NRChromaClamp", "NRTransferMode",
    "NRStyleHighlightGuard", "NRDetailStability", "NRResolutionScale", "NRFollowInputRes",
    "NRLookUpsample", "NRLookMode", "NRLookStrength", "NRLookBrighten", "NRLookDarken",
    "NRLookColour", "NRLookHue", "NRLookShadows", "NRLookMidtones", "NRLookHighlights",
    "NRLookMaxBrighten", "NRLookMaxDarken", "NRLookMaxColour", "NRLookTone", "NRLookDetail",
    "NRLookDetailRadius", "NRLookHalo", "NRLookHaloBrightSide", "NRLookStabilize",
    "NRLookStabilizeMs", "NRLookStabilizeDetail", "NRSdrBlackRestore", "NRDisplayPedestal",
    "NRCodecMode", "NRNeuralFloorGuard", "NRSourceEncoding", "NRLinearUnitNits",
    "NRProxyAnchor", "NRPaperWhiteScale", "NRDiffuseWhiteNits", "NRPQCalibration",
    "NRFeedMode", "NRPresentEncoding", "NRPresentWhiteNits", "NRPresentWhiteSource",
    "NRNormGovernor", "NRNormCutAware", "NRNormAttackStops", "NRNormReleaseStops",
    "NRNormSlewStops", "NRPresentGuides", "NRDepthMode", "NRMVecScaleX", "NRMVecScaleY",
    "NROutputRect", "NRPresentFrames", "NRPresentFgSource",
}

_WILSJO_KEYS = {
    "RunBeforeSR", "Passes", "UnlockPasses", "History", "PreHistory", "PostHistory", "Preset", "Style",
    "Intensity", "LocalStructure", "LocalTone", "SkinStructure", "AutoMask", "NoiseEveryFrame",
    "TransferStrength", "ColourStrength", "MaxRatio", "ReversibleMode", "ReplaceDetailStrength",
    "WorkingScale", "Transfer", "ScalingDownscaler", "SpatialCompression", "SpatialCenterX",
    "SpatialCenterY", "SpatialWorkX", "SpatialWorkY", "SpatialOffsetX", "SpatialOffsetY",
    "SpatialShiftX", "SpatialShiftY", "WhitePointSource", "WhitePointScale", "WhitePointTrim",
    "ExposureTrimAnchors", "AutoExposureTrim", "AutoExposureHighlightProtection", "AutoExposureTrimAnchors",
    "SkinProtection", "SkinDetail", "SkinColour", "EnvironmentDetail", "EnvironmentColour",
    "ClassicScaler", "Downscale", "PassesMode", "FastMethod", "FastEditStrength", "ColourLimit",
    "FastExtrapolationPasses", "FastExtrapolationHaloLimit", "FastExtrapolationFine",
    "FastFeedbackPasses", "FastFeedbackHaloLimit", "FastFeedbackFine",
    "FastHybridPasses", "FastHybridHaloLimit", "FastHybridFine", "FastInterleavedPasses", "FastInterleavedPreset",
    "FastInterleavedDepthDisocclusion",
}

_DYNAMIC_PASS_KEYS = {
    "Pass%d%s" % (p, field)
    for p in range(2, 31)
    for field in ("Preset", "Style", "Intensity", "LocalStructure", "LocalTone", "SkinStructure", "AutoMask")
    if not (field == "Preset" and p > 3)
}

_GLOBAL_FIELDS = {
    ("network", "model"),
    ("network", "motion without depth"),
    ("network", "resize history"),
    ("network", "history precision"),
    ("network", "style grade"),
    ("network", "optimizedlayers"),
    ("cc_silver_aardwold_optimized", "layers"),
    ("cc_small_fp8_optimized", "layers"),
    ("renodx", "enabled"),
    ("hudfix", "mode"),
    ("nrcontrol", "layers"),
}
for _k in ("Enabled", "Exposure", "ExposureBias", "Curve", "Contrast", "Saturation", "Inverse"):
    _GLOBAL_FIELDS.add(("preprocess", _k.casefold()))
for _k in _RENODX_KEYS:
    _GLOBAL_FIELDS.add(("renodx", _k.casefold()))
for _k in _WILSJO_KEYS | _DYNAMIC_PASS_KEYS:
    _GLOBAL_FIELDS.add(("wilsjo2", _k.casefold()))

_LAYER_FIELDS = {
    k.casefold()
    for k in ({"Enabled", "Processing", "Placement", "Network"} | _RENODX_KEYS | _WILSJO_KEYS | _DYNAMIC_PASS_KEYS)
}

_SMALL_GPU_NOTE_ZH = (
    "模型与显卡说明：预设使用 CC_SMALL_FP8_OPTIMIZED（Ultra Performance）；"
    "RX 9000 自动运行 Ultra Performance（INT4），RX 7000 运行时会自动回退至 Quality（无需手动更改）。"
)
_SMALL_GPU_NOTE_EN = (
    "Model & GPU note: Uses CC_SMALL_FP8_OPTIMIZED (Ultra Performance). "
    "RX 9000 runs Ultra Performance (INT4) directly; RX 7000 automatically falls back to Quality at runtime with no manual change required."
)
_SILVER_GPU_NOTE_ZH = (
    "模型与显卡说明：预设使用 CC_SILVER_AARDWOLD_OPTIMIZED（Ultra Performance）；"
    "RX 9000 与 RX 7000 均原生支持 Ultra Performance（INT4）。"
)
_SILVER_GPU_NOTE_EN = (
    "Model & GPU note: Uses CC_SILVER_AARDWOLD_OPTIMIZED (Ultra Performance). "
    "Both RX 9000 and RX 7000 natively support Ultra Performance (INT4)."
)

_GENERAL_GUIDE_ZH = (
    "调参建议（画面表现不理想时按需微调）：\n"
    "1. 人物阴影过黑：优先调低第 2 层（电影风格）的皮肤细节，仍偏黑再调第 1 层皮肤细节；若无效（部分游戏人物阴影归在环境细节中），改调低环境细节。\n"
    "2. 人物肤色过红：优先调低第 1 层（标准风格）的皮肤颜色，仍偏红再调第 2 层皮肤颜色；若无效则改调低环境颜色。\n"
    "3. 画面强度不够 / 过强：优先调整第 1 层 Pass 次数，再调整第 2 层 Pass 次数。\n"
    "4. 人物边缘白边（电影风格 + Fast 叠层特性）：适当降低第 2 层细节强度（会略微降低整体 NR 强度），仍明显再降低第 1 层细节强度。\n"
    "5. 分辨率与帧数：为兼顾 4K 与中端配置，第 2 层默认 75% 分辨率（帧数接近单层 100%，效果优于单层）；配置充足可将两层分辨率均调至 100%。"
)
_GENERAL_GUIDE_EN = (
    "Tuning guide (adjust if the image looks off in a specific game):\n"
    "1. Shadows on characters too dark: Lower Layer 2 (Cinematic) SkinDetail first, then Layer 1 SkinDetail; if ineffective, lower EnvironmentDetail (some games classify character shadows under environment).\n"
    "2. Skin tone too red: Lower Layer 1 (Standard) SkinColour first, then Layer 2 SkinColour; if ineffective, lower EnvironmentColour.\n"
    "3. Effect too weak or too strong: Adjust Layer 1 Passes first, then Layer 2 Passes.\n"
    "4. White edges around characters (Cinematic + Fast stacking): Lower Layer 2 detail strength slightly (reduces overall NR strength a little), then Layer 1 if still visible.\n"
    "5. Resolution & FPS: Layer 2 defaults to 75% scale for balanced 4K / mid-range performance (FPS near single-layer 100%, better visuals); raise both layers to 100% if you have GPU headroom."
)

PRESETS = (
    {
        "id": "al-feibi-wuwa-hyperreal",
        "author": "Al菲比",
        "titleZh": "鸣潮 · 单双层超写实质感增强",
        "titleEn": "Wuthering Waves · Dual-Layer Hyper-Real",
        "subtitleZh": "《鸣潮》特调双层超写实质感预设（wilsjo2 Present + RenoDX Before，新模型 Optimized-Ultra）",
        "subtitleEn": "Tuned dual-layer hyper-real preset for Wuthering Waves (wilsjo2 Present + RenoDX Before, Small Optimized-Ultra)",
        "summaryZh": "ASI 画面 · CC_SMALL_FP8_OPTIMIZED · 2 个图层 · wilsjo2 (Present) + RenoDX (Before)",
        "summaryEn": "ASI look · CC_SMALL_FP8_OPTIMIZED · 2 layers · wilsjo2 (Present) + RenoDX (Before)",
        "gameNoticeZh": "本预设为《鸣潮》特调，如果不是《鸣潮》，推荐尝试通用预设或其他预设。",
        "gameNoticeEn": "This preset is tuned specifically for Wuthering Waves. If this is not Wuthering Waves, try a general preset or another preset instead.",
        "tipsZh": (
            "游戏内滤镜推荐（全局滤镜 → 默认，调色盘 (0, 0)）：\n"
            "色温 -643K｜锐化 18｜亮度 -16｜对比度 20｜饱和度 -15｜灰阶 -20｜阴影 20｜泛光 0｜晕影 0｜颗粒 0。\n\n"
            "帧数优化建议：如果帧数不够，可适当降低第 1 层 NR 分辨率（0.69–1.0 内对画面影响较小）。\n\n"
            + _SMALL_GPU_NOTE_ZH
        ),
        "tipsEn": (
            "Recommended in-game filter (Global Filter -> Default, Palette (0, 0)):\n"
            "Color Temp -643K | Sharpening 18 | Brightness -16 | Contrast 20 | Saturation -15 | Grayscale -20 | Shadows 20 | Bloom 0 | Vignette 0 | Grain 0.\n\n"
            "FPS tip: If FPS is too low, reduce Layer 1 NR resolution scale (0.69-1.0 has minimal visual impact).\n\n"
            + _SMALL_GPU_NOTE_EN
        ),
        "requiredModels": ["CC_SMALL_FP8_OPTIMIZED.bin"],
        "iniSource": (
            "[Network]\n"
            "Model = CC_SMALL_FP8_OPTIMIZED\n"
            "Motion Without Depth = 1\n"
            "Resize History = 0\n"
            "Style Grade = 1\n"
            "History Precision = 16\n"
            "[CC_SMALL_FP8_OPTIMIZED]\n"
            "Layers = 3\n"
            "[CC_SILVER_AARDWOLD_OPTIMIZED]\n"
            "Layers = 3\n"
            "[Preprocess]\n"
            "Enabled = 0\n"
            "[RenoDX]\n"
            "Enabled = 1\n"
            "NeuralUplift = 1\n"
            "[HudFix]\n"
            "Mode = off\n"
            "[NRControl]\n"
            "Layers = 1\n"
            "[Layer1]\n"
            "Enabled = 1\n"
            "Processing = wilsjo2\n"
            "Placement = present\n"
            "Network = CC_SMALL_FP8_OPTIMIZED\n"
            "WorkingScale = 1.00\n"
            "LocalTone = 0.45\n"
            "PassesMode = fast\n"
            "FastMethod = 1\n"
            "FastExtrapolationHaloLimit = true\n"
            "FastExtrapolationPasses = 2.55\n"
            "AutoMask = 0\n"
            "SkinProtection = 1\n"
            "SkinDetail = 0.69\n"
            "SkinColour = 0.30\n"
            "LocalStructure = 1.05\n"
            "Intensity = 0.78\n"
            "ReplaceDetailStrength = 1.50\n"
            "AutoExposureTrim = 5.00\n"
            "AutoExposureHighlightProtection = 0\n"
            "EnvironmentDetail = 0.78\n"
            "Style = 0\n"
            "ColourLimit = false\n"
            "Passes = 2\n"
            "SpatialCompression = 1\n"
            "SpatialCenterX = 80.0\n"
            "SpatialCenterY = 80.0\n"
            "SpatialWorkX = 85.0\n"
            "SpatialWorkY = 85.0\n"
            "PreHistory = 1\n"
            "[Layer2]\n"
            "Enabled = 1\n"
            "Processing = RenoDX\n"
            "Placement = before\n"
            "Network = CC_SMALL_FP8_OPTIMIZED\n"
            "NRStyle = 2\n"
            "NRResolutionScale = 0.75\n"
            "NRLocalTone = 0.25\n"
            "NRPassesMode = fast\n"
            "NRFastMethod = 1\n"
            "NRFastExtrapolationHaloLimit = 1\n"
            "NRIntensity = 0.88\n"
            "NRLocalStructure = 0.91\n"
            "NRLookMode = 1\n"
            "NRLookStrength = 1.30\n"
            "NRFastExtrapolationPasses = 2.60\n"
            "NRLookBrighten = 1.35\n"
            "NRLookDarken = 0.55\n"
            "NRLookColour = 0.91\n"
            "NRLookShadows = 0.88\n"
            "NRLookMidtones = 1.25\n"
            "NRLookTone = 0.75\n"
            "NRLookDetail = 0.88\n"
            "NRLookHaloBrightSide = 0.30\n"
            "NRLookHalo = 0.10\n"
            "NRLookHighlights = 0.88\n"
            "NRAutoMask = 0\n"
            "NRFastExtrapolationFine = 0.00\n"
            "NRFastInterleavedPasses = 2\n"
            "NRDetailStability = 1\n"
        ),
    },
    {
        "id": "al-feibi-nte-hyperreal",
        "author": "Al菲比",
        "titleZh": "异环 · 单双层超写实质感增强",
        "titleEn": "Neverness to Everness · Dual-Layer Hyper-Real",
        "subtitleZh": "《异环》特调双层超写实质感预设（wilsjo2 Present + RenoDX Before，新模型 Optimized-Ultra）",
        "subtitleEn": "Tuned dual-layer hyper-real preset for Neverness to Everness (wilsjo2 Present + RenoDX Before, Small Optimized-Ultra)",
        "summaryZh": "ASI 画面 · CC_SMALL_FP8_OPTIMIZED · 2 个图层 · wilsjo2 (Present) + RenoDX (Before)",
        "summaryEn": "ASI look · CC_SMALL_FP8_OPTIMIZED · 2 layers · wilsjo2 (Present) + RenoDX (Before)",
        "gameNoticeZh": "本预设为《异环》特调，如果不是《异环》，推荐尝试通用预设或其他预设。",
        "gameNoticeEn": "This preset is tuned specifically for Neverness to Everness (NTE). If this is not NTE, try a general preset or another preset instead.",
        "tipsZh": (
            "帧数优化建议：如果帧数不够，可适当降低第 1、2 层 NR 分辨率（0.69–1.0 内对画面影响较小）。\n\n"
            + _SMALL_GPU_NOTE_ZH
        ),
        "tipsEn": (
            "FPS tip: If FPS is too low, reduce Layer 1 and Layer 2 NR resolution scale (0.69-1.0 has minimal visual impact).\n\n"
            + _SMALL_GPU_NOTE_EN
        ),
        "requiredModels": ["CC_SMALL_FP8_OPTIMIZED.bin"],
        "iniSource": (
            "[Network]\n"
            "Model = CC_SMALL_FP8_OPTIMIZED\n"
            "Motion Without Depth = 1\n"
            "Resize History = 0\n"
            "Style Grade = 1\n"
            "History Precision = 16\n"
            "[CC_SMALL_FP8_OPTIMIZED]\n"
            "Layers = 3\n"
            "[CC_SILVER_AARDWOLD_OPTIMIZED]\n"
            "Layers = 3\n"
            "[Preprocess]\n"
            "Enabled = 0\n"
            "[RenoDX]\n"
            "Enabled = 1\n"
            "NeuralUplift = 1\n"
            "[HudFix]\n"
            "Mode = off\n"
            "[NRControl]\n"
            "Layers = 1\n"
            "[Layer1]\n"
            "Enabled = 1\n"
            "Processing = wilsjo2\n"
            "Placement = present\n"
            "Network = CC_SMALL_FP8_OPTIMIZED\n"
            "WorkingScale = 0.85\n"
            "LocalTone = 0.30\n"
            "PassesMode = fast\n"
            "FastMethod = 1\n"
            "FastExtrapolationHaloLimit = true\n"
            "FastExtrapolationPasses = 2.55\n"
            "AutoMask = 0\n"
            "SkinProtection = 1\n"
            "SkinDetail = 0.69\n"
            "SkinColour = 0.25\n"
            "LocalStructure = 1.05\n"
            "Intensity = 0.78\n"
            "ReplaceDetailStrength = 1.50\n"
            "AutoExposureTrim = 5.00\n"
            "AutoExposureHighlightProtection = 0\n"
            "EnvironmentDetail = 0.80\n"
            "Style = 0\n"
            "SpatialCompression = 1\n"
            "SpatialWorkX = 85.1\n"
            "SpatialWorkY = 85.1\n"
            "[Layer2]\n"
            "Enabled = 1\n"
            "Processing = RenoDX\n"
            "Placement = before\n"
            "Network = CC_SMALL_FP8_OPTIMIZED\n"
            "NRStyle = 2\n"
            "NRResolutionScale = 1.00\n"
            "NRLocalTone = 0.20\n"
            "NRPassesMode = fast\n"
            "NRFastMethod = 1\n"
            "NRFastExtrapolationHaloLimit = 1\n"
            "NRIntensity = 0.88\n"
            "NRLocalStructure = 0.91\n"
            "NRLookMode = 1\n"
            "NRLookStrength = 1.45\n"
            "NRFastExtrapolationPasses = 2.53\n"
            "NRLookBrighten = 1.35\n"
            "NRLookDarken = 0.65\n"
            "NRLookColour = 0.91\n"
            "NRLookShadows = 0.88\n"
            "NRLookMidtones = 1.25\n"
            "NRLookTone = 0.75\n"
            "NRLookDetail = 0.88\n"
            "NRLookHaloBrightSide = 0.30\n"
            "NRLookHalo = 0.10\n"
            "NRLookHighlights = 0.88\n"
            "NRAutoMask = 0\n"
            "NRFastExtrapolationFine = 0.00\n"
            "NRFastInterleavedPasses = 2\n"
        ),
    },
    {
        "id": "al-feibi-general-new",
        "author": "Al菲比",
        "titleZh": "通用 · 单双层参数（新模型）",
        "titleEn": "General · Dual-Layer (New Model)",
        "subtitleZh": "多游戏通用双层预设（双层 wilsjo2，新模型 CC_SMALL_FP8_OPTIMIZED Ultra）",
        "subtitleEn": "General-purpose dual-layer preset (dual wilsjo2, CC_SMALL_FP8_OPTIMIZED Ultra)",
        "summaryZh": "ASI 画面 · CC_SMALL_FP8_OPTIMIZED · 2 个图层 · 双层 wilsjo2",
        "summaryEn": "ASI look · CC_SMALL_FP8_OPTIMIZED · 2 layers · dual wilsjo2",
        "gameNoticeZh": "",
        "gameNoticeEn": "",
        "tipsZh": _GENERAL_GUIDE_ZH + "\n\n" + _SMALL_GPU_NOTE_ZH,
        "tipsEn": _GENERAL_GUIDE_EN + "\n\n" + _SMALL_GPU_NOTE_EN,
        "requiredModels": ["CC_SMALL_FP8_OPTIMIZED.bin"],
        "iniSource": (
            "[Network]\n"
            "Model = CC_SMALL_FP8_OPTIMIZED\n"
            "Motion Without Depth = 1\n"
            "Resize History = 0\n"
            "Style Grade = 1\n"
            "History Precision = 16\n"
            "[CC_SMALL_FP8_OPTIMIZED]\n"
            "Layers = 3\n"
            "[CC_SILVER_AARDWOLD_OPTIMIZED]\n"
            "Layers = 3\n"
            "[Preprocess]\n"
            "Enabled = 0\n"
            "[RenoDX]\n"
            "Enabled = 1\n"
            "NeuralUplift = 1\n"
            "[HudFix]\n"
            "Mode = off\n"
            "[NRControl]\n"
            "Layers = 1\n"
            "[Layer1]\n"
            "Enabled = 1\n"
            "Processing = wilsjo2\n"
            "Placement = present\n"
            "Network = CC_SMALL_FP8_OPTIMIZED\n"
            "WorkingScale = 0.85\n"
            "LocalTone = 0.30\n"
            "PassesMode = fast\n"
            "FastMethod = 1\n"
            "FastExtrapolationHaloLimit = true\n"
            "FastExtrapolationPasses = 2.55\n"
            "AutoMask = 0\n"
            "SkinProtection = 1\n"
            "SkinDetail = 0.69\n"
            "SkinColour = 0.50\n"
            "LocalStructure = 1.05\n"
            "Intensity = 0.78\n"
            "ReplaceDetailStrength = 1.50\n"
            "AutoExposureTrim = 5.00\n"
            "AutoExposureHighlightProtection = 0\n"
            "EnvironmentDetail = 0.80\n"
            "Style = 0\n"
            "SpatialCompression = 1\n"
            "SpatialWorkX = 85.1\n"
            "SpatialWorkY = 85.1\n"
            "Transfer = 1\n"
            "FastExtrapolationFine = 0\n"
            "ColourStrength = 0.95\n"
            "[Layer2]\n"
            "Enabled = 1\n"
            "Processing = wilsjo2\n"
            "Placement = before\n"
            "Network = CC_SMALL_FP8_OPTIMIZED\n"
            "WorkingScale = 0.75\n"
            "Style = 2\n"
            "Intensity = 0.91\n"
            "LocalStructure = 1.00\n"
            "LocalTone = 0.30\n"
            "AutoMask = 0\n"
            "SkinProtection = 1\n"
            "PassesMode = fast\n"
            "FastMethod = 1\n"
            "FastExtrapolationPasses = 3.26\n"
            "SkinDetail = 0.80\n"
            "SkinColour = 0.60\n"
        ),
    },
    {
        "id": "al-feibi-general-old",
        "author": "Al菲比",
        "titleZh": "通用 · 单双层参数（旧模型）",
        "titleEn": "General · Dual-Layer (Legacy Model)",
        "subtitleZh": "多游戏通用双层预设（双层 wilsjo2，旧模型 CC_SILVER_AARDWOLD_OPTIMIZED Ultra）",
        "subtitleEn": "General-purpose dual-layer preset (dual wilsjo2, CC_SILVER_AARDWOLD_OPTIMIZED Ultra)",
        "summaryZh": "ASI 画面 · CC_SILVER_AARDWOLD_OPTIMIZED · 2 个图层 · 双层 wilsjo2",
        "summaryEn": "ASI look · CC_SILVER_AARDWOLD_OPTIMIZED · 2 layers · dual wilsjo2",
        "gameNoticeZh": "",
        "gameNoticeEn": "",
        "tipsZh": _GENERAL_GUIDE_ZH + "\n\n" + _SILVER_GPU_NOTE_ZH,
        "tipsEn": _GENERAL_GUIDE_EN + "\n\n" + _SILVER_GPU_NOTE_EN,
        "requiredModels": ["CC_SILVER_AARDWOLD_OPTIMIZED.bin"],
        "iniSource": (
            "[Network]\n"
            "Model = CC_SILVER_AARDWOLD_OPTIMIZED\n"
            "Motion Without Depth = 1\n"
            "Resize History = 0\n"
            "Style Grade = 1\n"
            "History Precision = 16\n"
            "[CC_SILVER_AARDWOLD_OPTIMIZED]\n"
            "Layers = 3\n"
            "[CC_SMALL_FP8_OPTIMIZED]\n"
            "Layers = 3\n"
            "[Preprocess]\n"
            "Enabled = 0\n"
            "[RenoDX]\n"
            "Enabled = 1\n"
            "NeuralUplift = 1\n"
            "[HudFix]\n"
            "Mode = off\n"
            "[NRControl]\n"
            "Layers = 1\n"
            "[Layer1]\n"
            "Enabled = 1\n"
            "Processing = wilsjo2\n"
            "Placement = present\n"
            "Network = CC_SILVER_AARDWOLD_OPTIMIZED\n"
            "WorkingScale = 1.00\n"
            "LocalTone = 0.25\n"
            "PassesMode = fast\n"
            "FastMethod = 1\n"
            "FastExtrapolationHaloLimit = true\n"
            "FastExtrapolationPasses = 3.23\n"
            "AutoMask = 0\n"
            "SkinProtection = 1\n"
            "SkinDetail = 0.55\n"
            "SkinColour = 0.30\n"
            "LocalStructure = 1.10\n"
            "Intensity = 0.88\n"
            "ReplaceDetailStrength = 1.50\n"
            "AutoExposureTrim = 5.00\n"
            "AutoExposureHighlightProtection = 0\n"
            "EnvironmentDetail = 0.80\n"
            "Style = 0\n"
            "SpatialCompression = 1\n"
            "SpatialWorkX = 85.1\n"
            "SpatialWorkY = 85.1\n"
            "Transfer = 1\n"
            "FastExtrapolationFine = 0\n"
            "ColourStrength = 0.95\n"
            "[Layer2]\n"
            "Enabled = 1\n"
            "Processing = wilsjo2\n"
            "Placement = before\n"
            "Network = CC_SILVER_AARDWOLD_OPTIMIZED\n"
            "WorkingScale = 0.75\n"
            "Style = 2\n"
            "Intensity = 0.91\n"
            "LocalStructure = 1.00\n"
            "LocalTone = 0.14\n"
            "AutoMask = 0\n"
            "SkinProtection = 1\n"
            "PassesMode = fast\n"
            "FastMethod = 1\n"
            "FastExtrapolationPasses = 3.26\n"
            "SkinDetail = 0.69\n"
            "SkinColour = 0.60\n"
            "EnvironmentDetail = 0.95\n"
        ),
    },
)

_BY_ID = {p["id"]: p for p in PRESETS}


def get_preset(preset_id: str) -> Optional[dict]:
    return _BY_ID.get((preset_id or "").strip())


def public_preset(preset: dict) -> dict:
    return {
        "id": preset["id"],
        "author": preset["author"],
        "titleZh": preset["titleZh"],
        "titleEn": preset["titleEn"],
        "subtitleZh": preset["subtitleZh"],
        "subtitleEn": preset["subtitleEn"],
        "summaryZh": preset["summaryZh"],
        "summaryEn": preset["summaryEn"],
        "gameNoticeZh": preset["gameNoticeZh"],
        "gameNoticeEn": preset["gameNoticeEn"],
        "tipsZh": preset["tipsZh"],
        "tipsEn": preset["tipsEn"],
        "requiredModels": list(preset["requiredModels"]),
    }


def public_list() -> list[dict]:
    return [public_preset(p) for p in PRESETS]


def _layer_number(section_name: str) -> Optional[int]:
    if len(section_name) < 6 or not section_name[:5].casefold() == "layer":
        return None
    rest = section_name[5:]
    if not rest.isdigit():
        return None
    num = int(rest)
    return num if num > 0 else None


def _parse_preset_source(ini_source: str):
    """Parse a built-in preset's INI source into ordered global section entries and numbered layer entries."""
    by_section: dict[str, tuple[str, list[tuple[str, str]]]] = {}
    layers: dict[int, list[tuple[str, str]]] = {}
    section = None
    layer_no = None
    for raw in ini_source.splitlines():
        line = raw.strip()
        if not line or line[0] in ";#":
            continue
        hm = _HEADER_RE.match(line)
        if hm:
            section = hm.group(1).strip()
            layer_no = _layer_number(section)
            if layer_no is not None:
                layers[layer_no] = []
            continue
        if section is None:
            continue
        am = _ASSIGN_RE.match(line)
        if not am:
            continue
        key, value = am.group(2).strip(), am.group(4).strip()
        if layer_no is not None:
            if key.casefold() in _LAYER_FIELDS:
                layers[layer_no].append((key, value))
        else:
            sec_low = section.casefold()
            if (sec_low, key.casefold()) in _GLOBAL_FIELDS:
                if sec_low not in by_section:
                    by_section[sec_low] = (section, [])
                by_section[sec_low][1].append((key, value))
    return by_section, layers


def apply_preset_bytes(existing: bytes, preset: dict) -> bytes:
    """Apply ``preset`` onto an existing ``dlssnr-amd.ini`` byte buffer without touching user hotkeys or comments."""
    if existing.startswith(b"\xef\xbb\xbf"):
        existing = existing[3:]
    text = existing.decode("utf-8", errors="surrogateescape")
    nl = "\r\n" if "\r\n" in text else "\n"
    by_section, layers = _parse_preset_source(preset["iniSource"])
    remaining = {k: (v[0], list(v[1])) for k, v in by_section.items()}

    out: list[str] = []
    layer_locals: dict[int, list[str]] = {}
    section: Optional[str] = None
    in_layer = False
    layer_no = 0

    for raw in text.splitlines(True):
        stripped = raw.rstrip("\r\n")
        ending = raw[len(stripped):]
        hm = _HEADER_RE.match(stripped)
        if hm:
            section = hm.group(1).strip()
            lno = _layer_number(section)
            if lno is not None:
                in_layer = True
                layer_no = lno
                layer_locals[layer_no] = []
                continue
            in_layer = False
            out.append(stripped + (ending or nl))
            sec_low = section.casefold()
            if sec_low in remaining:
                _, entries = remaining.pop(sec_low)
                for key, val in entries:
                    out.append("%s=%s%s" % (key, val, nl))
            continue

        if in_layer:
            am = _ASSIGN_RE.match(stripped)
            if am:
                key = am.group(2).strip()
                if key.casefold() not in _LAYER_FIELDS:
                    layer_locals[layer_no].append(raw)
            else:
                layer_locals[layer_no].append(raw)
            continue

        if section is not None:
            am = _ASSIGN_RE.match(stripped)
            if am:
                key = am.group(2).strip()
                if (section.casefold(), key.casefold()) in _GLOBAL_FIELDS:
                    out.append(";" + stripped + (ending or nl))
                    continue

        out.append(raw)

    if out and not out[-1].endswith(("\n", "\r")):
        out.append(nl)

    for _, (sec_name, entries) in remaining.items():
        out.append("[%s]%s" % (sec_name, nl))
        for key, val in entries:
            out.append("%s=%s%s" % (key, val, nl))

    for num in sorted(set(layer_locals.keys()) | set(layers.keys())):
        locals_list = layer_locals.get(num, [])
        if num not in layers:
            if not locals_list:
                continue
            out.append("; [Layer%d] retired; local settings kept (inactive)%s" % (num, nl))
            for raw in locals_list:
                line = "; " + raw
                if not line.endswith(("\n", "\r")):
                    line += nl
                out.append(line)
            continue
        out.append("[Layer%d]%s" % (num, nl))
        for raw in locals_list:
            line = raw
            if not line.endswith(("\n", "\r")):
                line += nl
            out.append(line)
        for key, val in layers[num]:
            out.append("%s=%s%s" % (key, val, nl))

    return "".join(out).encode("utf-8", errors="surrogateescape")


def missing_models(game_dir: Path, preset: dict) -> list[str]:
    model_dir = Path(game_dir) / "dlssnr-amd"
    return [name for name in preset["requiredModels"] if not (model_dir / name).is_file()]


def apply_preset_to_game(game_dir: Path, preset_id: str) -> dict:
    """Atomically back up and update ``<game_dir>/dlssnr-amd.ini`` with a built-in preset."""
    preset = get_preset(preset_id)
    if preset is None:
        raise ValueError("unknown preset: %s" % preset_id)
    game = Path(game_dir)
    target = game / "dlssnr-amd.ini"
    if not target.is_file() or target.is_symlink():
        raise FileNotFoundError("dlssnr-amd.ini not found in %s" % game)
    original = target.read_bytes()
    updated = apply_preset_bytes(original, preset)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")[:-3]
    backup_name = "dlssnr-amd.ini.bak-%s-%s" % (stamp, secrets.token_hex(4))
    backup_path = game / backup_name

    # Write backup first, then stage and atomically replace dlssnr-amd.ini.
    fd_bak, tmp_bak = tempfile.mkstemp(dir=str(game), prefix=".dlssnr-bak-")
    try:
        with os.fdopen(fd_bak, "wb") as f:
            f.write(original)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp_bak, backup_path)
    except Exception:
        try:
            os.unlink(tmp_bak)
        except OSError:
            pass
        raise

    fd, tmp = tempfile.mkstemp(dir=str(game), prefix=".dlssnr-stage-")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(updated)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, target)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise

    return {
        "backup": backup_name,
        "preset": public_preset(preset),
        "missingModels": missing_models(game, preset),
    }
