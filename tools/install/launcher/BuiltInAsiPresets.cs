using System;
using System.Text;

namespace OptScalerNR.Launcher
{
    public sealed class BuiltInAsiPreset
    {
        public readonly string Id;
        public readonly string Author;
        public readonly string TitleZh;
        public readonly string TitleEn;
        public readonly string SubtitleZh;
        public readonly string SubtitleEn;
        public readonly string GameNoticeZh;
        public readonly string GameNoticeEn;
        public readonly string TipsZh;
        public readonly string TipsEn;
        public readonly string IniSource;
        string _shareCode;

        public string Tag { get { return BuiltInAsiPresets.TagPrefix + Id; } }

        public BuiltInAsiPreset(
            string id, string author,
            string titleZh, string titleEn,
            string subtitleZh, string subtitleEn,
            string gameNoticeZh, string gameNoticeEn,
            string tipsZh, string tipsEn,
            string iniSource)
        {
            Id = id;
            Author = author;
            TitleZh = titleZh;
            TitleEn = titleEn;
            SubtitleZh = subtitleZh;
            SubtitleEn = subtitleEn;
            GameNoticeZh = gameNoticeZh ?? "";
            GameNoticeEn = gameNoticeEn ?? "";
            TipsZh = tipsZh ?? "";
            TipsEn = tipsEn ?? "";
            IniSource = iniSource;
        }

        public string DropdownLabel(bool chinese)
        {
            return (chinese ? "[预设] " + TitleZh : "[Preset] " + TitleEn) + " (" + Author + ")";
        }

        public string GetShareCode()
        {
            if (_shareCode == null)
                _shareCode = MochizukiShareCode.Encode(Encoding.UTF8.GetBytes(IniSource), null, Author + " - " + TitleZh);
            return _shareCode;
        }
    }

    public static class BuiltInAsiPresets
    {
        public const string TagPrefix = "builtin:";

        const string SmallOptimizedGpuNoteZh =
            "模型与显卡说明：预设使用 CC_SMALL_FP8_OPTIMIZED（Ultra Performance）；RX 9000 自动运行 Ultra Performance（INT4），RX 7000 运行时会自动回退至 Quality（无需手动更改）。";
        const string SmallOptimizedGpuNoteEn =
            "Model & GPU note: Uses CC_SMALL_FP8_OPTIMIZED (Ultra Performance). RX 9000 runs Ultra Performance (INT4) directly; RX 7000 automatically falls back to Quality at runtime with no manual change required.";

        const string SilverOptimizedGpuNoteZh =
            "模型与显卡说明：预设使用 CC_SILVER_AARDWOLD_OPTIMIZED（Ultra Performance）；RX 9000 与 RX 7000 均原生支持 Ultra Performance（INT4）。";
        const string SilverOptimizedGpuNoteEn =
            "Model & GPU note: Uses CC_SILVER_AARDWOLD_OPTIMIZED (Ultra Performance). Both RX 9000 and RX 7000 natively support Ultra Performance (INT4).";

        const string GeneralTuningGuideZh =
            "调参建议（画面表现不理想时按需微调）：\n" +
            "1. 人物阴影过黑：优先调低第 2 层（电影风格）的皮肤细节，仍偏黑再调第 1 层皮肤细节；若无效（部分游戏人物阴影归在环境细节中），改调低环境细节。\n" +
            "2. 人物肤色过红：优先调低第 1 层（标准风格）的皮肤颜色，仍偏红再调第 2 层皮肤颜色；若无效则改调低环境颜色。\n" +
            "3. 画面强度不够 / 过强：优先调整第 1 层 Pass 次数，再调整第 2 层 Pass 次数。\n" +
            "4. 人物边缘白边（电影风格 + Fast 叠层特性）：适当降低第 2 层细节强度（会略微降低整体 NR 强度），仍明显再降低第 1 层细节强度。\n" +
            "5. 分辨率与帧数：为兼顾 4K 与中端配置，第 2 层默认 75% 分辨率（帧数接近单层 100%，效果优于单层）；配置充足可将两层分辨率均调至 100%。";

        const string GeneralTuningGuideEn =
            "Tuning guide (adjust if the image looks off in a specific game):\n" +
            "1. Shadows on characters too dark: Lower Layer 2 (Cinematic) SkinDetail first, then Layer 1 SkinDetail; if ineffective, lower EnvironmentDetail (some games classify character shadows under environment).\n" +
            "2. Skin tone too red: Lower Layer 1 (Standard) SkinColour first, then Layer 2 SkinColour; if ineffective, lower EnvironmentColour.\n" +
            "3. Effect too weak or too strong: Adjust Layer 1 Passes first, then Layer 2 Passes.\n" +
            "4. White edges around characters (Cinematic + Fast stacking): Lower Layer 2 detail strength slightly (reduces overall NR strength a little), then Layer 1 if still visible.\n" +
            "5. Resolution & FPS: Layer 2 defaults to 75% scale for balanced 4K / mid-range performance (FPS near single-layer 100%, better visuals); raise both layers to 100% if you have GPU headroom.";

        public static readonly BuiltInAsiPreset[] All = new BuiltInAsiPreset[]
        {
            new BuiltInAsiPreset(
                "al-feibi-wuwa-hyperreal",
                "Al菲比",
                "鸣潮 · 单双层超写实质感增强",
                "Wuthering Waves · Dual-Layer Hyper-Real",
                "《鸣潮》特调双层超写实质感预设（wilsjo2 Present + RenoDX Before，新模型 Optimized-Ultra）",
                "Tuned dual-layer hyper-real preset for Wuthering Waves (wilsjo2 Present + RenoDX Before, Small Optimized-Ultra)",
                "本预设为《鸣潮》特调，如果不是《鸣潮》，推荐尝试通用预设或其他预设。",
                "This preset is tuned specifically for Wuthering Waves. If this is not Wuthering Waves, try a general preset or another preset instead.",
                "游戏内滤镜推荐（全局滤镜 → 默认，调色盘 (0, 0)）：\n" +
                "色温 -643K｜锐化 18｜亮度 -16｜对比度 20｜饱和度 -15｜灰阶 -20｜阴影 20｜泛光 0｜晕影 0｜颗粒 0。\n\n" +
                "帧数优化建议：如果帧数不够，可适当降低第 1 层 NR 分辨率（0.69–1.0 内对画面影响较小）。\n\n" +
                SmallOptimizedGpuNoteZh,
                "Recommended in-game filter (Global Filter -> Default, Palette (0, 0)):\n" +
                "Color Temp -643K | Sharpening 18 | Brightness -16 | Contrast 20 | Saturation -15 | Grayscale -20 | Shadows 20 | Bloom 0 | Vignette 0 | Grain 0.\n\n" +
                "FPS tip: If FPS is too low, reduce Layer 1 NR resolution scale (0.69-1.0 has minimal visual impact).\n\n" +
                SmallOptimizedGpuNoteEn,
                "[Network]\n" +
                "Model = CC_SMALL_FP8_OPTIMIZED\n" +
                "Motion Without Depth = 1\n" +
                "Resize History = 0\n" +
                "Style Grade = 1\n" +
                "History Precision = 16\n" +
                "[CC_SMALL_FP8_OPTIMIZED]\n" +
                "Layers = 3\n" +
                "[CC_SILVER_AARDWOLD_OPTIMIZED]\n" +
                "Layers = 3\n" +
                "[Preprocess]\n" +
                "Enabled = 0\n" +
                "[RenoDX]\n" +
                "Enabled = 1\n" +
                "NeuralUplift = 1\n" +
                "[HudFix]\n" +
                "Mode = off\n" +
                "[NRControl]\n" +
                "Layers = 1\n" +
                "[Layer1]\n" +
                "Enabled = 1\n" +
                "Processing = wilsjo2\n" +
                "Placement = present\n" +
                "Network = CC_SMALL_FP8_OPTIMIZED\n" +
                "WorkingScale = 1.00\n" +
                "LocalTone = 0.45\n" +
                "PassesMode = fast\n" +
                "FastMethod = 1\n" +
                "FastExtrapolationHaloLimit = true\n" +
                "FastExtrapolationPasses = 2.55\n" +
                "AutoMask = 0\n" +
                "SkinProtection = 1\n" +
                "SkinDetail = 0.69\n" +
                "SkinColour = 0.30\n" +
                "LocalStructure = 1.05\n" +
                "Intensity = 0.78\n" +
                "ReplaceDetailStrength = 1.50\n" +
                "AutoExposureTrim = 5.00\n" +
                "AutoExposureHighlightProtection = 0\n" +
                "EnvironmentDetail = 0.78\n" +
                "Style = 0\n" +
                "ColourLimit = false\n" +
                "Passes = 2\n" +
                "SpatialCompression = 1\n" +
                "SpatialCenterX = 80.0\n" +
                "SpatialCenterY = 80.0\n" +
                "SpatialWorkX = 85.0\n" +
                "SpatialWorkY = 85.0\n" +
                "PreHistory = 1\n" +
                "[Layer2]\n" +
                "Enabled = 1\n" +
                "Processing = RenoDX\n" +
                "Placement = before\n" +
                "Network = CC_SMALL_FP8_OPTIMIZED\n" +
                "NRStyle = 2\n" +
                "NRResolutionScale = 0.75\n" +
                "NRLocalTone = 0.25\n" +
                "NRPassesMode = fast\n" +
                "NRFastMethod = 1\n" +
                "NRFastExtrapolationHaloLimit = 1\n" +
                "NRIntensity = 0.88\n" +
                "NRLocalStructure = 0.91\n" +
                "NRLookMode = 1\n" +
                "NRLookStrength = 1.30\n" +
                "NRFastExtrapolationPasses = 2.60\n" +
                "NRLookBrighten = 1.35\n" +
                "NRLookDarken = 0.55\n" +
                "NRLookColour = 0.91\n" +
                "NRLookShadows = 0.88\n" +
                "NRLookMidtones = 1.25\n" +
                "NRLookTone = 0.75\n" +
                "NRLookDetail = 0.88\n" +
                "NRLookHaloBrightSide = 0.30\n" +
                "NRLookHalo = 0.10\n" +
                "NRLookHighlights = 0.88\n" +
                "NRAutoMask = 0\n" +
                "NRFastExtrapolationFine = 0.00\n" +
                "NRFastInterleavedPasses = 2\n" +
                "NRDetailStability = 1\n"
            ),
            new BuiltInAsiPreset(
                "al-feibi-nte-hyperreal",
                "Al菲比",
                "异环 · 单双层超写实质感增强",
                "Neverness to Everness · Dual-Layer Hyper-Real",
                "《异环》特调双层超写实质感预设（wilsjo2 Present + RenoDX Before，新模型 Optimized-Ultra）",
                "Tuned dual-layer hyper-real preset for Neverness to Everness (wilsjo2 Present + RenoDX Before, Small Optimized-Ultra)",
                "本预设为《异环》特调，如果不是《异环》，推荐尝试通用预设或其他预设。",
                "This preset is tuned specifically for Neverness to Everness (NTE). If this is not NTE, try a general preset or another preset instead.",
                "帧数优化建议：如果帧数不够，可适当降低第 1、2 层 NR 分辨率（0.69–1.0 内对画面影响较小）。\n\n" +
                SmallOptimizedGpuNoteZh,
                "FPS tip: If FPS is too low, reduce Layer 1 and Layer 2 NR resolution scale (0.69-1.0 has minimal visual impact).\n\n" +
                SmallOptimizedGpuNoteEn,
                "[Network]\n" +
                "Model = CC_SMALL_FP8_OPTIMIZED\n" +
                "Motion Without Depth = 1\n" +
                "Resize History = 0\n" +
                "Style Grade = 1\n" +
                "History Precision = 16\n" +
                "[CC_SMALL_FP8_OPTIMIZED]\n" +
                "Layers = 3\n" +
                "[CC_SILVER_AARDWOLD_OPTIMIZED]\n" +
                "Layers = 3\n" +
                "[Preprocess]\n" +
                "Enabled = 0\n" +
                "[RenoDX]\n" +
                "Enabled = 1\n" +
                "NeuralUplift = 1\n" +
                "[HudFix]\n" +
                "Mode = off\n" +
                "[NRControl]\n" +
                "Layers = 1\n" +
                "[Layer1]\n" +
                "Enabled = 1\n" +
                "Processing = wilsjo2\n" +
                "Placement = present\n" +
                "Network = CC_SMALL_FP8_OPTIMIZED\n" +
                "WorkingScale = 0.85\n" +
                "LocalTone = 0.30\n" +
                "PassesMode = fast\n" +
                "FastMethod = 1\n" +
                "FastExtrapolationHaloLimit = true\n" +
                "FastExtrapolationPasses = 2.55\n" +
                "AutoMask = 0\n" +
                "SkinProtection = 1\n" +
                "SkinDetail = 0.69\n" +
                "SkinColour = 0.25\n" +
                "LocalStructure = 1.05\n" +
                "Intensity = 0.78\n" +
                "ReplaceDetailStrength = 1.50\n" +
                "AutoExposureTrim = 5.00\n" +
                "AutoExposureHighlightProtection = 0\n" +
                "EnvironmentDetail = 0.80\n" +
                "Style = 0\n" +
                "SpatialCompression = 1\n" +
                "SpatialWorkX = 85.1\n" +
                "SpatialWorkY = 85.1\n" +
                "[Layer2]\n" +
                "Enabled = 1\n" +
                "Processing = RenoDX\n" +
                "Placement = before\n" +
                "Network = CC_SMALL_FP8_OPTIMIZED\n" +
                "NRStyle = 2\n" +
                "NRResolutionScale = 1.00\n" +
                "NRLocalTone = 0.20\n" +
                "NRPassesMode = fast\n" +
                "NRFastMethod = 1\n" +
                "NRFastExtrapolationHaloLimit = 1\n" +
                "NRIntensity = 0.88\n" +
                "NRLocalStructure = 0.91\n" +
                "NRLookMode = 1\n" +
                "NRLookStrength = 1.45\n" +
                "NRFastExtrapolationPasses = 2.53\n" +
                "NRLookBrighten = 1.35\n" +
                "NRLookDarken = 0.65\n" +
                "NRLookColour = 0.91\n" +
                "NRLookShadows = 0.88\n" +
                "NRLookMidtones = 1.25\n" +
                "NRLookTone = 0.75\n" +
                "NRLookDetail = 0.88\n" +
                "NRLookHaloBrightSide = 0.30\n" +
                "NRLookHalo = 0.10\n" +
                "NRLookHighlights = 0.88\n" +
                "NRAutoMask = 0\n" +
                "NRFastExtrapolationFine = 0.00\n" +
                "NRFastInterleavedPasses = 2\n"
            ),
            new BuiltInAsiPreset(
                "al-feibi-general-new",
                "Al菲比",
                "通用 · 单双层参数（新模型）",
                "General · Dual-Layer (New Model)",
                "多游戏通用双层预设（双层 wilsjo2，新模型 CC_SMALL_FP8_OPTIMIZED Ultra）",
                "General-purpose dual-layer preset (dual wilsjo2, CC_SMALL_FP8_OPTIMIZED Ultra)",
                "",
                "",
                GeneralTuningGuideZh + "\n\n" + SmallOptimizedGpuNoteZh,
                GeneralTuningGuideEn + "\n\n" + SmallOptimizedGpuNoteEn,
                "[Network]\n" +
                "Model = CC_SMALL_FP8_OPTIMIZED\n" +
                "Motion Without Depth = 1\n" +
                "Resize History = 0\n" +
                "Style Grade = 1\n" +
                "History Precision = 16\n" +
                "[CC_SMALL_FP8_OPTIMIZED]\n" +
                "Layers = 3\n" +
                "[CC_SILVER_AARDWOLD_OPTIMIZED]\n" +
                "Layers = 3\n" +
                "[Preprocess]\n" +
                "Enabled = 0\n" +
                "[RenoDX]\n" +
                "Enabled = 1\n" +
                "NeuralUplift = 1\n" +
                "[HudFix]\n" +
                "Mode = off\n" +
                "[NRControl]\n" +
                "Layers = 1\n" +
                "[Layer1]\n" +
                "Enabled = 1\n" +
                "Processing = wilsjo2\n" +
                "Placement = present\n" +
                "Network = CC_SMALL_FP8_OPTIMIZED\n" +
                "WorkingScale = 0.85\n" +
                "LocalTone = 0.30\n" +
                "PassesMode = fast\n" +
                "FastMethod = 1\n" +
                "FastExtrapolationHaloLimit = true\n" +
                "FastExtrapolationPasses = 2.55\n" +
                "AutoMask = 0\n" +
                "SkinProtection = 1\n" +
                "SkinDetail = 0.69\n" +
                "SkinColour = 0.50\n" +
                "LocalStructure = 1.05\n" +
                "Intensity = 0.78\n" +
                "ReplaceDetailStrength = 1.50\n" +
                "AutoExposureTrim = 5.00\n" +
                "AutoExposureHighlightProtection = 0\n" +
                "EnvironmentDetail = 0.80\n" +
                "Style = 0\n" +
                "SpatialCompression = 1\n" +
                "SpatialWorkX = 85.1\n" +
                "SpatialWorkY = 85.1\n" +
                "Transfer = 1\n" +
                "FastExtrapolationFine = 0\n" +
                "ColourStrength = 0.95\n" +
                "[Layer2]\n" +
                "Enabled = 1\n" +
                "Processing = wilsjo2\n" +
                "Placement = before\n" +
                "Network = CC_SMALL_FP8_OPTIMIZED\n" +
                "WorkingScale = 0.75\n" +
                "Style = 2\n" +
                "Intensity = 0.91\n" +
                "LocalStructure = 1.00\n" +
                "LocalTone = 0.30\n" +
                "AutoMask = 0\n" +
                "SkinProtection = 1\n" +
                "PassesMode = fast\n" +
                "FastMethod = 1\n" +
                "FastExtrapolationPasses = 3.26\n" +
                "SkinDetail = 0.80\n" +
                "SkinColour = 0.60\n"
            ),
            new BuiltInAsiPreset(
                "al-feibi-general-old",
                "Al菲比",
                "通用 · 单双层参数（旧模型）",
                "General · Dual-Layer (Legacy Model)",
                "多游戏通用双层预设（双层 wilsjo2，旧模型 CC_SILVER_AARDWOLD_OPTIMIZED Ultra）",
                "General-purpose dual-layer preset (dual wilsjo2, CC_SILVER_AARDWOLD_OPTIMIZED Ultra)",
                "",
                "",
                GeneralTuningGuideZh + "\n\n" + SilverOptimizedGpuNoteZh,
                GeneralTuningGuideEn + "\n\n" + SilverOptimizedGpuNoteEn,
                "[Network]\n" +
                "Model = CC_SILVER_AARDWOLD_OPTIMIZED\n" +
                "Motion Without Depth = 1\n" +
                "Resize History = 0\n" +
                "Style Grade = 1\n" +
                "History Precision = 16\n" +
                "[CC_SILVER_AARDWOLD_OPTIMIZED]\n" +
                "Layers = 3\n" +
                "[CC_SMALL_FP8_OPTIMIZED]\n" +
                "Layers = 3\n" +
                "[Preprocess]\n" +
                "Enabled = 0\n" +
                "[RenoDX]\n" +
                "Enabled = 1\n" +
                "NeuralUplift = 1\n" +
                "[HudFix]\n" +
                "Mode = off\n" +
                "[NRControl]\n" +
                "Layers = 1\n" +
                "[Layer1]\n" +
                "Enabled = 1\n" +
                "Processing = wilsjo2\n" +
                "Placement = present\n" +
                "Network = CC_SILVER_AARDWOLD_OPTIMIZED\n" +
                "WorkingScale = 1.00\n" +
                "LocalTone = 0.25\n" +
                "PassesMode = fast\n" +
                "FastMethod = 1\n" +
                "FastExtrapolationHaloLimit = true\n" +
                "FastExtrapolationPasses = 3.23\n" +
                "AutoMask = 0\n" +
                "SkinProtection = 1\n" +
                "SkinDetail = 0.55\n" +
                "SkinColour = 0.30\n" +
                "LocalStructure = 1.10\n" +
                "Intensity = 0.88\n" +
                "ReplaceDetailStrength = 1.50\n" +
                "AutoExposureTrim = 5.00\n" +
                "AutoExposureHighlightProtection = 0\n" +
                "EnvironmentDetail = 0.80\n" +
                "Style = 0\n" +
                "SpatialCompression = 1\n" +
                "SpatialWorkX = 85.1\n" +
                "SpatialWorkY = 85.1\n" +
                "Transfer = 1\n" +
                "FastExtrapolationFine = 0\n" +
                "ColourStrength = 0.95\n" +
                "[Layer2]\n" +
                "Enabled = 1\n" +
                "Processing = wilsjo2\n" +
                "Placement = before\n" +
                "Network = CC_SILVER_AARDWOLD_OPTIMIZED\n" +
                "WorkingScale = 0.75\n" +
                "Style = 2\n" +
                "Intensity = 0.91\n" +
                "LocalStructure = 1.00\n" +
                "LocalTone = 0.14\n" +
                "AutoMask = 0\n" +
                "SkinProtection = 1\n" +
                "PassesMode = fast\n" +
                "FastMethod = 1\n" +
                "FastExtrapolationPasses = 3.26\n" +
                "SkinDetail = 0.69\n" +
                "SkinColour = 0.60\n" +
                "EnvironmentDetail = 0.95\n"
            )
        };

        public static bool IsBuiltInTag(string tag)
        {
            return !string.IsNullOrEmpty(tag) && tag.StartsWith(TagPrefix, StringComparison.OrdinalIgnoreCase);
        }

        public static BuiltInAsiPreset FindByTag(string tag)
        {
            if (!IsBuiltInTag(tag)) return null;
            var id = tag.Substring(TagPrefix.Length);
            for (var i = 0; i < All.Length; i++)
                if (string.Equals(All[i].Id, id, StringComparison.OrdinalIgnoreCase)) return All[i];
            return null;
        }
    }
}
