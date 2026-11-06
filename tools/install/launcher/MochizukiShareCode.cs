using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace OptScalerNR.Launcher
{
    // MZ4: section+key style code for the standalone dlssnr-amd.asi.
    // MZ1/2/3 targeted the retired built-in runtime and are rejected, not migrated.
    public static class MochizukiShareCode
    {
        public sealed class CodeException : Exception
        {
            public readonly string Reason;
            public CodeException(string reason) : base(reason) { Reason = reason; }
        }
        sealed class Field
        {
            public readonly string Section, Key, Kind;
            public Field(string section, string key, string kind) { Section = section; Key = key; Kind = kind; }
        }
        // kind: str, num, bool. "auto" is always accepted for num/bool.
        static readonly Field[] GlobalFields = BuildGlobal();
        static readonly Field[] LayerFields = BuildLayers();
        static Field F(string section, string key, string kind) { return new Field(section, key, kind); }
        static Field[] BuildGlobal()
        {
            var list = new List<Field>();
            list.Add(F("Network", "Model", "str"));
            list.Add(F("Network", "Motion Without Depth", "bool"));
            list.Add(F("Network", "Resize History", "bool"));
            list.Add(F("Network", "History Precision", "history"));
            list.Add(F("Network", "Style Grade", "bool"));
            list.Add(F("Network", "OptimizedLayers", "optimized_layers"));
            list.Add(F("RenoDX", "Enabled", "bool"));
            foreach (var key in new[] { "Enabled", "Exposure", "ExposureBias", "Curve", "Contrast", "Saturation", "Inverse" })
                list.Add(F("Preprocess", key, key == "Enabled" || key == "Inverse" ? "bool" : (key == "Exposure" || key == "Curve") ? "str" : "num"));
            foreach (var key in RenoDXKeys()) list.Add(F("RenoDX", key, KindOfRenoDX(key)));
            foreach (var key in WilsjoKeys()) list.Add(F("wilsjo2", key, KindOfWilsjo(key)));
            foreach (var key in DynamicPassKeys()) list.Add(F("wilsjo2", key, KindOfWilsjo(key)));
            list.Add(F("HudFix", "Mode", "hudfix"));
            list.Add(F("NRControl", "Layers", "bool"));
            return list.ToArray();
        }
        static Field[] BuildLayers()
        {
            var list = new List<Field>();
            foreach (var key in new[] { "Enabled", "Processing", "Placement", "Network" })
                list.Add(F("Layer", key, key == "Enabled" ? "bool" : key == "Placement" ? "placement" : "str"));
            foreach (var key in RenoDXKeys()) list.Add(F("Layer", key, KindOfRenoDX(key)));
            foreach (var key in WilsjoKeys()) list.Add(F("Layer", key, KindOfWilsjo(key)));
            foreach (var key in DynamicPassKeys()) list.Add(F("Layer", key, KindOfWilsjo(key)));
            return list.ToArray();
        }
        static string KindOfRenoDX(string key)
        {
            switch (key) {
                case "NRPassesMode": return "str";
                case "NeuralUplift": case "NRAutoMask": case "NRSkinIndependent":
                case "NRUnlockPasses": case "NRChainedHistory": case "NRNoiseEveryFrame":
                case "NRPass2FollowPass1": case "NRPass2AutoMask": case "NRPass3FollowPass1":
                case "NRPass3AutoMask": case "NRPass4FollowPass1": case "NRPass4AutoMask":
                case "NRLookMode": case "NRLookStabilizeDetail": case "NRNeuralFloorGuard":
                case "NRNormCutAware":
                case "NRFastExtrapolationHaloLimit": case "NRFastFeedbackHaloLimit":
                case "NRFastHybridHaloLimit":
                    return "bool";
                default: return "num";
            }
        }
        static string KindOfWilsjo(string key)
        {
            if (key.EndsWith("AutoMask", StringComparison.OrdinalIgnoreCase)) return "bool";
            switch (key) {
                case "ExposureTrimAnchors": case "AutoExposureTrimAnchors": return "str";
                case "RunBeforeSR": case "UnlockPasses": case "AutoMask": case "NoiseEveryFrame":
                case "SpatialCompression": case "SkinProtection": case "ColourLimit":
                case "FastExtrapolationHaloLimit": case "FastFeedbackHaloLimit": case "FastHybridHaloLimit":
                    return "bool";
                case "ClassicScaler": case "Downscale": case "PassesMode": case "FastMethod":
                    return "str";
                default: return "num";
            }
        }
        static string[] DynamicPassKeys()
        {
            var keys = new List<string>();
            string[] fields = { "Preset", "Style", "Intensity", "LocalStructure", "LocalTone", "SkinStructure", "AutoMask" };
            for (int pass = 2; pass <= 30; ++pass)
                foreach (var field in fields)
                {
                    if (field == "Preset" && pass > 3) continue;
                    keys.Add("Pass" + pass.ToString(Invariant) + field);
                }
            return keys.ToArray();
        }
        static string[] RenoDXKeys()
        {
            return new[] {
                "NeuralUplift", "NRHookPoint", "NRIntensity", "NRStyle", "NRLocalTone", "NRLocalStructure",
                "NRSkinStructure", "NRAutoMask", "NRSkinIndependent", "NRPassesMode", "NRFastMethod",
                "NRFastExtrapolationPasses", "NRFastExtrapolationHaloLimit", "NRFastExtrapolationFine",
                "NRFastFeedbackPasses", "NRFastFeedbackHaloLimit", "NRFastFeedbackFine",
                "NRFastHybridPasses", "NRFastHybridHaloLimit", "NRFastHybridFine", "NRFastEditStrength",
                "NRPasses", "NRUnlockPasses", "NRChainedHistory", "NRNoiseEveryFrame",
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
                "NROutputRect", "NRPresentFrames", "NRPresentFgSource"
            };
        }
        static string[] WilsjoKeys()
        {
            return new[] {
                "RunBeforeSR", "Passes", "UnlockPasses", "History", "Preset", "Style",
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
                "FastHybridPasses", "FastHybridHaloLimit", "FastHybridFine"
            };
        }
        static Field FindField(IEnumerable<Field> fields, string section, string key)
        {
            foreach (var f in fields)
                if (string.Equals(f.Section, section, StringComparison.OrdinalIgnoreCase) &&
                    string.Equals(f.Key, key, StringComparison.OrdinalIgnoreCase)) return f;
            return null;
        }
        public sealed class Entry
        {
            public int Layer; // 0 = global
            public string Section, Key, Value, OldValue;
            public bool Reset; // current value that the import will restore to its default
        }
        public sealed class DecodedCode
        {
            public readonly List<Entry> Entries = new List<Entry>();
            public readonly SortedSet<int> LayerNumbers = new SortedSet<int>();
            public string GameTag = "", Note = "";
            public int LayerCount { get { return LayerNumbers.Count; } }
            public bool LayersEnabled {
                get {
                    foreach (var e in Entries)
                        if (e.Layer == 0 && string.Equals(e.Section, "NRControl", StringComparison.OrdinalIgnoreCase) &&
                            string.Equals(e.Key, "Layers", StringComparison.OrdinalIgnoreCase))
                            return IsTrue(e.Value);
                    return false;
                }
            }
            public int FirstLayer { get { return LayerNumbers.Count == 0 ? 0 : LayerNumbers.Min; } }
            public string Network { get { foreach (var e in Entries) if (e.Layer == 0 && string.Equals(e.Key, "Model", StringComparison.OrdinalIgnoreCase)) return e.Value; return ""; } }
        }
        static readonly CultureInfo Invariant = CultureInfo.InvariantCulture;
        static readonly Regex Header = new Regex(@"^\s*\[([^\]]+)\]\s*(?:[;#].*)?$");
        static readonly Regex Assignment = new Regex(@"^(\s*)([^=;#]+?)(\s*=\s*)([^;#]*)(.*)$");
        public static ushort Crc16(string payload)
        {
            ushort crc = 0xffff;
            foreach (byte b in Encoding.UTF8.GetBytes(payload)) {
                crc ^= (ushort)(b << 8);
                for (int i = 0; i < 8; i++) crc = (ushort)((crc & 0x8000) != 0 ? (crc << 1) ^ 0x1021 : crc << 1);
            }
            return crc;
        }
        public static string NormalizeGameTag(string game)
        {
            if (string.IsNullOrEmpty(game)) return "";
            var name = Path.GetFileName(game);
            return name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? name.Substring(0, name.Length - 4) : name;
        }
        public static string SanitizeNote(string note)
        {
            if (string.IsNullOrEmpty(note)) return "";
            var clean = new StringBuilder(note.Length);
            foreach (var ch in note.Trim())
            {
                if (ch == '\r' || ch == '\n' || char.IsControl(ch)) continue;
                clean.Append(ch);
                if (clean.Length >= 60) break;
            }
            return clean.ToString().Trim();
        }
        // Fields are joined by '|' and the whole payload ends at ';CRC'. Encode
        // delimiters inside game tags/notes so a note such as "warm|night" or
        // "warm;night" cannot split the protocol or stop GUI extraction.
        static string EscapeField(string value)
        {
            if (string.IsNullOrEmpty(value)) return "";
            return value.Replace("~", "~7E").Replace("|", "~7C").Replace(";", "~3B");
        }
        static string UnescapeField(string value)
        {
            if (string.IsNullOrEmpty(value)) return "";
            return value.Replace("~3B", ";").Replace("~7C", "|").Replace("~7E", "~");
        }
        public static bool GameTagMatches(string tag, string executable)
        { return string.Equals(NormalizeGameTag(tag), NormalizeGameTag(executable), StringComparison.OrdinalIgnoreCase); }
        static void ValidateValue(Field field, string value)
        {
            if (value == null) throw new CodeException("value");
            value = value.Trim();
            if (value.Length == 0) throw new CodeException("value");
            if (string.Equals(value, "auto", StringComparison.OrdinalIgnoreCase)) return;
            if (field.Kind == "str") return;
            if (field.Kind == "bool") {
                if (value == "0" || value == "1" ||
                    string.Equals(value, "true", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(value, "false", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(value, "yes", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(value, "no", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(value, "on", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(value, "off", StringComparison.OrdinalIgnoreCase)) return;
                throw new CodeException("value");
            }
            if (field.Kind == "history") {
                var h = value.ToLowerInvariant();
                if (h == "16" || h == "fp16" || h == "half" || h == "rgba16f" ||
                    h == "32" || h == "fp32" || h == "full" || h == "rgba32f") return;
                throw new CodeException("value");
            }
            if (field.Kind == "optimized_layers") {
                var o = value.ToLowerInvariant();
                if (o == "0" || o == "1" || o == "2" || o == "3" ||
                    o == "quality" || o == "balanced" || o == "performance" ||
                    o == "ultra" || o == "ultra performance" || o == "ultra_performance") return;
                throw new CodeException("value");
            }
            if (field.Kind == "processing") {
                var p = value.ToLowerInvariant();
                if (p == "renodx" || p == "wilsjo2") return;
                throw new CodeException("value");
            }
            if (field.Kind == "hudfix") {
                var h = value.ToLowerInvariant();
                if (h == "off" || h == "auto" || h == "game" || h == "captured" || h == "estimated" ||
                    h == "0" || h == "1" || h == "2" || h == "3" || h == "4") return;
                throw new CodeException("value");
            }
            if (field.Kind == "placement") {
                var p = value.ToLowerInvariant();
                if (p == "before" || p == "after" || p == "present" ||
                    p == "true" || p == "false" || p == "0" || p == "1") return;
                throw new CodeException("value");
            }
            decimal parsed;
            if (!decimal.TryParse(value, NumberStyles.Float, Invariant, out parsed)) throw new CodeException("value");
        }
        static bool IsLayerSection(string name, out int layer)
        {
            layer = 0;
            if (name.Length < 6 || !name.StartsWith("Layer", StringComparison.OrdinalIgnoreCase)) return false;
            if (!int.TryParse(name.Substring(5), NumberStyles.None, Invariant, out layer) || layer <= 0) return false;
            return true;
        }
        static string NormalizeValue(string value) { return (value ?? "").Trim(); }
        static SortedDictionary<string, string> ReadStyle(byte[] ini)
        {
            var doc = ReadDocument(ini);
            var values = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            string section = null;
            foreach (var raw in SplitLines(doc.Text))
            {
                string text = raw.TrimEnd('\r', '\n');
                var header = Header.Match(text);
                if (header.Success) {
                    section = header.Groups[1].Value.Trim();
                    int resetLayer;
                    if (IsLayerSection(section, out resetLayer)) {
                        string prefix = "L" + resetLayer.ToString(Invariant) + ":";
                        var stale = new List<string>();
                        foreach (var existing in values.Keys) if (existing.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) stale.Add(existing);
                        foreach (var existing in stale) values.Remove(existing);
                    }
                    continue;
                }
                if (section == null) continue;
                var assignment = Assignment.Match(text);
                if (!assignment.Success) continue;
                string key = assignment.Groups[2].Value.Trim(), value = assignment.Groups[4].Value.Trim();
                int layer;
                if (IsLayerSection(section, out layer)) {
                    if (FindField(LayerFields, "Layer", key) == null) continue;
                    values["L" + layer + ":" + key] = value;
                } else {
                    if (FindField(GlobalFields, section, key) == null) continue;
                    values["G:" + section.ToUpperInvariant() + ":" + key] = value;
                }
            }
            return values;
        }
        public static string Encode(byte[] ini, string game = null, string note = null)
        {
            var doc = ReadDocument(ini);
            // Consumer INI semantics: the last assignment wins. Keep one effective
            // value per section/key (and per layer/key), preserving first-seen order
            // for a stable export; duplicate sections merge the same way.
            var globals = new Dictionary<string, string[]>(StringComparer.OrdinalIgnoreCase);
            var globalOrder = new List<string>();
            var layerValues = new SortedDictionary<int, Dictionary<string, string[]>>();
            var layerOrder = new SortedDictionary<int, List<string>>();
            string section = null;
            foreach (var raw in SplitLines(doc.Text))
            {
                string text = raw.TrimEnd('\r', '\n');
                var header = Header.Match(text);
                if (header.Success) {
                    section = header.Groups[1].Value.Trim();
                    int resetLayer;
                    if (IsLayerSection(section, out resetLayer)) {
                        // read_chain keeps the last whole [LayerN] section: a later
                        // header replaces the earlier one, including omitted keys.
                        layerValues.Remove(resetLayer);
                        layerOrder.Remove(resetLayer);
                    }
                    continue;
                }
                if (section == null) continue;
                var assignment = Assignment.Match(text);
                if (!assignment.Success) continue;
                string key = assignment.Groups[2].Value.Trim(), value = assignment.Groups[4].Value.Trim();
                int layer;
                if (IsLayerSection(section, out layer)) {
                    if (FindField(LayerFields, "Layer", key) == null) continue;
                    Dictionary<string, string[]> values;
                    if (!layerValues.TryGetValue(layer, out values)) {
                        layerValues[layer] = values = new Dictionary<string, string[]>(StringComparer.OrdinalIgnoreCase);
                        layerOrder[layer] = new List<string>();
                    }
                    string id = key.ToUpperInvariant();
                    if (!values.ContainsKey(id)) layerOrder[layer].Add(id);
                    values[id] = new[] { key, value };
                } else {
                    if (FindField(GlobalFields, section, key) == null) continue;
                    string id = section.ToUpperInvariant() + "\t" + key.ToUpperInvariant();
                    if (!globals.ContainsKey(id)) globalOrder.Add(id);
                    globals[id] = new[] { section, key, value };
                }
            }
            var lines = new List<string>();
            foreach (var id in globalOrder) {
                var e = globals[id];
                lines.Add("G\t" + e[0] + "\t" + e[1] + "\t" + e[2]);
            }
            foreach (var layer in layerValues) {
                foreach (var id in layerOrder[layer.Key]) {
                    var e = layer.Value[id];
                    lines.Add("L\t" + layer.Key.ToString(Invariant) + "\t" + e[0] + "\t" + e[1]);
                }
            }
            string payload = "1|" + EscapeField(NormalizeGameTag(game)) + "|" + EscapeField(SanitizeNote(note)) + "|" +
                             Convert.ToBase64String(Encoding.UTF8.GetBytes(string.Join("\n", lines.ToArray())));
            return "MZ4:" + payload + ";" + Crc16(payload).ToString("X4", Invariant);
        }
        public static DecodedCode Decode(string code)
        {
            if (code == null) throw new CodeException("prefix");
            if (code.StartsWith("MZ1:", StringComparison.Ordinal) || code.StartsWith("MZ2:", StringComparison.Ordinal) ||
                code.StartsWith("MZ3:", StringComparison.Ordinal)) throw new CodeException("old");
            if (!code.StartsWith("MZ4:", StringComparison.Ordinal)) throw new CodeException("prefix");
            return DecodeBase64(code);
        }
        static DecodedCode DecodeBase64(string code)
        {
            if (code == null) throw new CodeException("prefix");
            if (code.StartsWith("MZ1:", StringComparison.Ordinal) || code.StartsWith("MZ2:", StringComparison.Ordinal) ||
                code.StartsWith("MZ3:", StringComparison.Ordinal)) throw new CodeException("old");
            if (!code.StartsWith("MZ4:", StringComparison.Ordinal)) throw new CodeException("prefix");
            int end = code.LastIndexOf(';');
            ushort crc;
            if (end < 4 || code.Length != end + 5 || !ushort.TryParse(code.Substring(end + 1), NumberStyles.AllowHexSpecifier, Invariant, out crc))
                throw new CodeException("format");
            string payload = code.Substring(4, end - 4);
            if (Crc16(payload) != crc) throw new CodeException("crc");
            string[] parts = payload.Split(new[] { '|' }, 4);
            if (parts.Length != 4 || parts[0] != "1") throw new CodeException("version");
            var decoded = new DecodedCode { GameTag = UnescapeField(parts[1]), Note = UnescapeField(parts[2]) };
            byte[] bytes;
            try { bytes = Convert.FromBase64String(parts[3]); }
            catch { throw new CodeException("format"); }
            string text;
            try { text = new UTF8Encoding(false, true).GetString(bytes); }
            catch { throw new CodeException("format"); }
            if (text.Length == 0) return decoded;
            foreach (var line in text.Split('\n'))
            {
                if (line.Length == 0) continue;
                var cols = line.Split('\t');
                if (cols.Length != 4) throw new CodeException("format");
                var entry = new Entry { Value = cols[3] };
                if (cols[0] == "G") {
                    entry.Layer = 0; entry.Section = cols[1]; entry.Key = cols[2];
                    var field = FindField(GlobalFields, entry.Section, entry.Key);
                    if (field == null) throw new CodeException("unknown");
                    ValidateValue(field, entry.Value);
                } else if (cols[0] == "L") {
                    int layer;
                    if (!int.TryParse(cols[1], NumberStyles.None, Invariant, out layer) || layer <= 0) throw new CodeException("layer");
                    entry.Layer = layer; entry.Section = "Layer" + layer.ToString(Invariant); entry.Key = cols[2];
                    var field = FindField(LayerFields, "Layer", entry.Key);
                    if (field == null) throw new CodeException("unknown");
                    ValidateValue(field, entry.Value);
                    decoded.LayerNumbers.Add(layer);
                } else throw new CodeException("format");
                decoded.Entries.Add(entry);
            }
            return decoded;
        }
        static string EntryValue(DecodedCode code, int layer, string key)
        {
            foreach (var entry in code.Entries)
                if (entry.Layer == layer && string.Equals(entry.Key, key, StringComparison.OrdinalIgnoreCase))
                    return entry.Value;
            return "";
        }
        static readonly Regex CodeInText = new Regex(@"MZ[1-4]:[^\r\n]*?;[0-9A-Fa-f]{4}(?![0-9A-Fa-f])", RegexOptions.Compiled);
        public static string ExtractCode(string text)
        {
            if (text == null) return null;
            var match = CodeInText.Match(text);
            return match.Success ? match.Value : null;
        }
        static bool IsTrue(string value)
        {
            string v = (value ?? "").Trim();
            return v == "1" || string.Equals(v, "true", StringComparison.OrdinalIgnoreCase) ||
                   string.Equals(v, "yes", StringComparison.OrdinalIgnoreCase) ||
                   string.Equals(v, "on", StringComparison.OrdinalIgnoreCase);
        }
        static bool LayerIsEnabled(DecodedCode code, int layer)
        {
            string value = EntryValue(code, layer, "Enabled");
            if (value.Length == 0) return true;
            value = value.Trim();
            return !(value == "0" || string.Equals(value, "false", StringComparison.OrdinalIgnoreCase) ||
                     string.Equals(value, "no", StringComparison.OrdinalIgnoreCase) ||
                     string.Equals(value, "off", StringComparison.OrdinalIgnoreCase));
        }
        public static string Summary(DecodedCode code, bool chinese)
        {
            bool hasLayers = code.LayerCount > 0;
            var active = new List<int>();
            if (hasLayers) {
                if (code.LayersEnabled) {
                    foreach (var layer in code.LayerNumbers) if (LayerIsEnabled(code, layer)) active.Add(layer);
                } else if (LayerIsEnabled(code, code.FirstLayer)) {
                    active.Add(code.FirstLayer);
                }
            }
            int activeLayers = active.Count;
            var text = new StringBuilder();
            if (hasLayers && activeLayers == 0) {
                text.Append(chinese ? "ASI 画面 · 0 个图层 · 不运行" : "ASI look · 0 layers · not running");
                if (code.GameTag.Length > 0) text.Append(chinese ? " · 来自 " : " · from ").Append(code.GameTag);
                return text.ToString();
            }
            int effectiveLayer = activeLayers > 0 ? active[0] : 0;
            string model = hasLayers
                ? EntryValue(code, effectiveLayer, "Network")
                : code.Network;
            if (model.Length == 0) model = code.Network;
            if (model.Length == 0) model = chinese ? "未指定网络" : "no network";
            text.Append(chinese ? "ASI 画面" : "ASI look").Append(" · ").Append(model)
                .Append(" · ").Append(chinese ? activeLayers + " 个图层" : activeLayers + " layer(s)");
            if (code.GameTag.Length > 0) text.Append(chinese ? " · 来自 " : " · from ").Append(code.GameTag);
            if (hasLayers) {
                string processing = EntryValue(code, effectiveLayer, "Processing");
                string placement = EntryValue(code, effectiveLayer, "Placement");
                if (processing.Length > 0 || placement.Length > 0) {
                    text.Append(" · ").Append(chinese ? "运行位置 " : "placement ")
                        .Append(chinese ? PlacementText(placement) : placement)
                        .Append(chinese ? " · 处理 " : " · process ").Append(processing);
                }
                if (code.LayerCount > activeLayers)
                    text.Append(chinese ? " · 文件另有 " + (code.LayerCount - activeLayers) + " 层未启用"
                                        : " · " + (code.LayerCount - activeLayers) + " more layer(s) disabled");
                else if (activeLayers > 1)
                    text.Append(chinese ? " · 等 " + activeLayers + " 层" : " · " + activeLayers + " layers total");
            } else {
                string hook = EntryValue(code, 0, "NRHookPoint");
                string runBefore = EntryValue(code, 0, "RunBeforeSR");
                if (hook.Length > 0) text.Append(chinese ? " · RenoDX 挂钩点 " : " · RenoDX hook ").Append(hook);
                if (runBefore.Length > 0) text.Append(" · ").Append(chinese ? "wilsjo2 超分前 " : "wilsjo2 before SR ").Append(runBefore);
            }
            return text.ToString();
        }
        static string PlacementText(string placement)
        {
            string v = (placement ?? "").Trim().ToLowerInvariant();
            if (v == "before") return "超分前";
            if (v == "after") return "超分后";
            if (v == "present") return "最佳效果";
            return placement;
        }
        public static List<Entry> ChangedEntries(byte[] ini, DecodedCode code)
        {
            var current = ReadStyle(ini);
            var target = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var changed = new List<Entry>();
            foreach (var entry in code.Entries) {
                string id = entry.Layer == 0
                    ? "G:" + entry.Section.ToUpperInvariant() + ":" + entry.Key
                    : "L" + entry.Layer.ToString(Invariant) + ":" + entry.Key;
                target.Add(id);
                string value;
                if (!current.TryGetValue(id, out value)) {
                    changed.Add(entry);
                } else if (NormalizeValue(value) != NormalizeValue(entry.Value)) {
                    entry.OldValue = value;
                    changed.Add(entry);
                }
            }
            foreach (var pair in current) {
                if (target.Contains(pair.Key)) continue;
                if (pair.Key.StartsWith("G:", StringComparison.Ordinal)) {
                    var parts = pair.Key.Split(new[] { ':' }, 3);
                    if (parts.Length != 3) continue;
                    changed.Add(new Entry { Section = parts[1], Key = parts[2], Value = pair.Value, Reset = true });
                } else if (pair.Key.StartsWith("L", StringComparison.Ordinal)) {
                    int colon = pair.Key.IndexOf(':');
                    int layer;
                    if (colon <= 1 || !int.TryParse(pair.Key.Substring(1, colon - 1), NumberStyles.None, Invariant, out layer)) continue;
                    changed.Add(new Entry { Layer = layer, Section = "Layer", Key = pair.Key.Substring(colon + 1), Value = pair.Value, Reset = true });
                }
            }
            return changed;
        }
        public static List<Entry> LimitEntries(IList<Entry> entries, int limit)
        {
            var result = new List<Entry>();
            if (entries == null || limit <= 0) return result;
            int count = Math.Min(limit, entries.Count);
            for (int i = 0; i < count; ++i) result.Add(entries[i]);
            return result;
        }
        public static int NonDefaultCount(DecodedCode code) { return code.Entries.Count; }
        public static int DifferenceCount(byte[] ini, string code)
        {
            var before = ReadStyle(ini);
            var after = Decode(code);
            var target = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var entry in after.Entries)
            {
                string key = entry.Layer == 0
                    ? "G:" + entry.Section.ToUpperInvariant() + ":" + entry.Key
                    : "L" + entry.Layer.ToString(Invariant) + ":" + entry.Key;
                target[key] = entry.Value;
            }
            var all = new HashSet<string>(before.Keys, StringComparer.OrdinalIgnoreCase);
            all.UnionWith(target.Keys);
            int count = 0;
            foreach (var key in all)
            {
                string a, b;
                if (!before.TryGetValue(key, out a)) a = "";
                if (!target.TryGetValue(key, out b)) b = "";
                if (NormalizeValue(a) != NormalizeValue(b)) count++;
            }
            return count;
        }
        static void ValidateCodeEntries(DecodedCode code) { /* Decode already validates. */ }
        public static byte[] Apply(byte[] ini, string code)
        {
            var decoded = Decode(code);
            var bySection = new Dictionary<string, List<Entry>>(StringComparer.OrdinalIgnoreCase);
            var layers = new SortedDictionary<int, List<Entry>>();
            foreach (var entry in decoded.Entries)
            {
                if (entry.Layer == 0) {
                    List<Entry> list;
                    if (!bySection.TryGetValue(entry.Section, out list)) bySection[entry.Section] = list = new List<Entry>();
                    list.Add(entry);
                } else {
                    List<Entry> list;
                    if (!layers.TryGetValue(entry.Layer, out list)) layers[entry.Layer] = list = new List<Entry>();
                    list.Add(entry);
                }
            }
            var doc = ReadDocument(ini);
            string nl = Newline(doc.Text);
            var output = new StringBuilder();
            // Local layer keys (not part of the style schema) and comments survive a
            // replacement. A retired layer keeps them in its own number, disabled.
            var layerLocals = new SortedDictionary<int, List<string>>();
            string section = null;
            bool inLayer = false;
            int layerNo = 0;
            foreach (var raw in SplitLines(doc.Text))
            {
                string text = raw.TrimEnd('\r', '\n');
                string ending = raw.Substring(text.Length);
                var header = Header.Match(text);
                if (header.Success)
                {
                    section = header.Groups[1].Value.Trim();
                    if (IsLayerSection(section, out layerNo))
                    {
                        inLayer = true;
                        // The last whole section is the effective layer; do not merge
                        // local keys/comments from an earlier duplicate.
                        layerLocals[layerNo] = new List<string>();
                        continue;
                    }
                    inLayer = false;
                    output.Append(text).Append(ending.Length > 0 ? ending : nl);
                    if (bySection.ContainsKey(section)) {
                        foreach (var entry in bySection[section])
                            output.Append(entry.Key).Append('=').Append(entry.Value).Append(nl);
                        bySection.Remove(section);
                    }
                    continue;
                }
                if (inLayer)
                {
                    var assignment = Assignment.Match(text);
                    if (assignment.Success) {
                        string key = assignment.Groups[2].Value.Trim();
                        // Drop old style values; keep local non-style keys (e.g. ToggleKey) and comments.
                        if (FindField(LayerFields, "Layer", key) == null) layerLocals[layerNo].Add(raw);
                    } else layerLocals[layerNo].Add(raw);
                    continue;
                }
                var globalAssignment = section == null ? Match.Empty : Assignment.Match(text);
                if (globalAssignment.Success) {
                    string key = globalAssignment.Groups[2].Value.Trim();
                    if (FindField(GlobalFields, section, key) != null) {
                        output.Append(';').Append(text).Append(ending.Length > 0 ? ending : nl);
                        continue;
                    }
                }
                output.Append(raw);
            }
            if (output.Length > 0 && output[output.Length - 1] != '\n' && output[output.Length - 1] != '\r') output.Append(nl);
            foreach (var pair in bySection)
            {
                output.Append('[').Append(pair.Key).Append(']').Append(nl);
                foreach (var entry in pair.Value) output.Append(entry.Key).Append('=').Append(entry.Value).Append(nl);
            }
            var allLayers = new SortedSet<int>(layerLocals.Keys);
            allLayers.UnionWith(layers.Keys);
            foreach (var number in allLayers)
            {
                bool hasNew = layers.ContainsKey(number);
                List<string> locals;
                if (!layerLocals.TryGetValue(number, out locals)) locals = new List<string>();
                if (!hasNew)
                {
                    if (locals.Count == 0) continue;
                    // A retired layer must not stay active: read_chain treats any
                    // [LayerN] as a layer, which would hide a global import and
                    // could bind its ToggleKey. Keep the local content readable
                    // but commented out.
                    output.Append("; [Layer").Append(number.ToString(Invariant))
                        .Append("] retired; local settings kept (inactive)").Append(nl);
                    foreach (var raw in locals)
                    {
                        output.Append("; ").Append(raw);
                        if (raw.Length == 0 || (raw[raw.Length - 1] != '\n' && raw[raw.Length - 1] != '\r')) output.Append(nl);
                    }
                    continue;
                }
                output.Append("[Layer").Append(number.ToString(Invariant)).Append(']').Append(nl);
                foreach (var raw in locals)
                {
                    output.Append(raw);
                    if (raw.Length == 0 || (raw[raw.Length - 1] != '\n' && raw[raw.Length - 1] != '\r')) output.Append(nl);
                }
                foreach (var entry in layers[number]) output.Append(entry.Key).Append('=').Append(entry.Value).Append(nl);
            }
            return doc.Bytes(output.ToString());
        }
        static string Newline(string text)
        { return text.Contains("\r\n") ? "\r\n" : text.Contains("\n") ? "\n" : text.Contains("\r") ? "\r" : Environment.NewLine; }
        static IEnumerable<string> SplitLines(string text)
        { foreach (Match m in Regex.Matches(text, @"[^\r\n]*(?:\r\n|\r|\n|$)")) { if (m.Value.Length == 0) continue; yield return m.Value; } }
        sealed class Document
        {
            public string Text; Encoding encoding; byte[] preamble;
            public static Document Read(byte[] bytes)
            {
                Encoding encoding = new UTF8Encoding(false, true); int offset = 0;
                if (bytes.Length >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) offset = 3;
                else if (bytes.Length >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) { encoding = new UnicodeEncoding(false, false, true); offset = 2; }
                else if (bytes.Length >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) { encoding = new UnicodeEncoding(true, false, true); offset = 2; }
                var preamble = new byte[offset]; Buffer.BlockCopy(bytes, 0, preamble, 0, offset);
                try { return new Document { Text = encoding.GetString(bytes, offset, bytes.Length - offset), encoding = encoding, preamble = preamble }; }
                catch (DecoderFallbackException) { throw new CodeException("encoding"); }
            }
            public byte[] Bytes(string text)
            {
                byte[] content = encoding.GetBytes(text), result = new byte[preamble.Length + content.Length];
                Buffer.BlockCopy(preamble, 0, result, 0, preamble.Length); Buffer.BlockCopy(content, 0, result, preamble.Length, content.Length); return result;
            }
        }
        static Document ReadDocument(byte[] bytes) { return Document.Read(bytes); }
    }
}
