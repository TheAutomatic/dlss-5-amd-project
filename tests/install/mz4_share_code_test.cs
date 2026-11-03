using System;
using System.Text;
using OptScalerNR.Launcher;

static class Mz4ShareCodeTest
{
    static void Check(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
    }
    static string Text(byte[] bytes) { return new UTF8Encoding(false).GetString(bytes); }
    static void Main()
    {
        string source =
            "[Network]\nModel = CC_SMALL_FP8\nStyle Grade = false\nMotion Without Depth = 0\nResize History = 1\nHistory Precision = rgba32f\nHotkey = Ctrl+F11\nResidency = auto\n" +
            "[Preprocess]\nEnabled = 1\nExposure = auto\nExposureBias = -0.25\nCurve = filmic\n" +
            "[RenoDX]\nEnabled = 1\nNRIntensity = 1.25\nNRPassesMode = legacy\nNRChrome = 0\n" +
            "[wilsjo2]\nPasses = auto\nStyle = 2\nRunBeforeSR = true\nAutoExposureTrimAnchors = 0.0:1.0\n" +
            "[HudFix]\nMode = captured\nShowMask = false\n" +
            "[NRControl]\nLayers = false\nMenuKey = End\n" +
            "[Layer1]\nEnabled = 1\nProcessing = RenoDX\nPlacement = present\nNetwork = CC_SMALL_FP8_EXPERIMENTAL\nNRIntensity = 0.75\n" +
            "[Layer2]\nEnabled = 0\nProcessing = wilsjo2\nPlacement = after\nNetwork = CC_SILVER_AARDWOLD\nPasses = 2\n";
        string code = MochizukiShareCode.Encode(Encoding.UTF8.GetBytes(source), "pbz.exe", "night look");
        Check(code.StartsWith("MZ4:", StringComparison.Ordinal) && !code.Contains("\n"), "single-line code");
        var decoded = MochizukiShareCode.Decode(code);
        Check(decoded.GameTag == "pbz", "game tag");
        Check(decoded.Note == "night look", "note");
        Check(decoded.LayerCount == 2, "layers");
        bool sawAuto = false, sawRunBefore = false, sawAnchors = false;
        foreach (var entry in decoded.Entries) {
            if (entry.Key == "Passes" && entry.Value == "auto") sawAuto = true;
            if (entry.Key == "RunBeforeSR" && entry.Value == "true") sawRunBefore = true;
            if (entry.Key == "AutoExposureTrimAnchors" && entry.Value == "0.0:1.0") sawAnchors = true;
        }
        string changedSummary = MochizukiShareCode.Summary(decoded, true);
        Check(!changedSummary.Contains("\n") && changedSummary.Contains("图层"), "compact summary");
        var changed = MochizukiShareCode.ChangedEntries(
            Encoding.UTF8.GetBytes("[Network]\nModel = CC_SMALL_FP8\n[Preprocess]\nContrast = 1.30\n[Layer1]\nNRIntensity = 0.10\n"),
            decoded);
        bool sawChangedLayer = false;
        foreach (var e in changed) if (e.Layer == 1 && e.Key == "NRIntensity") sawChangedLayer = true;
        Check(sawChangedLayer, "import preview reports changed layer parameter");
        string oldLayerValue = "";
        foreach (var e in changed) if (e.Layer == 1 && e.Key == "NRIntensity") oldLayerValue = e.OldValue;
        Check(oldLayerValue == "0.10", "diff keeps the old value: " + oldLayerValue);
        bool sawReset = false;
        foreach (var e in changed) if (e.Reset && e.Key == "Contrast") sawReset = true;
        Check(sawReset, "import preview reports reset parameters");
        var many = new System.Collections.Generic.List<MochizukiShareCode.Entry>();
        for (int i = 0; i < 35; ++i) many.Add(new MochizukiShareCode.Entry { Section = "RenoDX", Key = "K" + i, Value = "1" });
        var limited = MochizukiShareCode.LimitEntries(many, 30);
        Check(limited.Count == 30 && limited[29].Key == "K29" && !limited.Exists(e => e.Key == "K30"), "preview limits direct rows");
        Check(sawAuto, "auto preserved");
        Check(sawRunBefore, "bool true preserved");
        Check(sawAnchors, "anchor string preserved");

        string target =
            "[Network]\nModel = CC_SILVER_AARDWOLD\nHotkey = Ctrl+F12\n" +
            "[Layer1]\n; local layer note\nToggleKey = F9\nEnabled = 1\nNRIntensity = 9.9\n" +
            "[Layer3]\n; retired local note\nToggleKey = E\nEnabled = 1\nProcessing = RenoDX\nNetwork = CC_SMALL_FP8\n" +
            "[Inspect]\nCompare = 1\n" +
            "[Log]\nEnabled = 1\n";
        string applied = Text(MochizukiShareCode.Apply(Encoding.UTF8.GetBytes(target), code));
        Check(applied.Contains("[Layer1]") && applied.Contains("[Layer2]"), "layers written");
        Check(applied.Contains("; local layer note") && applied.Contains("ToggleKey = F9"), "same-layer local keys kept");
        Check(applied.Contains("; [Layer3] retired") && applied.Contains("; ToggleKey = E"), "retired layer archived");
        Check(!System.Text.RegularExpressions.Regex.IsMatch(applied, @"(?m)^\[Layer3\]"), "retired layer inactive");
        Check(!applied.Contains("NRIntensity = 9.9"), "old style value replaced");
        Check(applied.Contains("History Precision=rgba32f"), "history precision string");
        Check(applied.Contains("Motion Without Depth=0") && applied.Contains("Resize History=1"), "network bools");
        Check(applied.Contains("Style Grade=false") && applied.Contains("Layers=false"), "global bool fields");
        Check(applied.Contains("Model=CC_SMALL_FP8"), "global model applied");
        Check(applied.Contains("Hotkey = Ctrl+F12"), "unrelated key preserved");
        Check(applied.Contains("[Inspect]") && applied.Contains("Compare = 1"), "inspect preserved");
        Check(applied.Contains("Passes=auto"), "auto written");
        Check(applied.Contains("NRChrome = 0") == false, "unknown source key not exported");

        string noLayerSource = "[Network]\nModel = CC_SILVER_AARDWOLD\n[wilsjo2]\nPasses = 1\n";
        string noLayerCode = MochizukiShareCode.Encode(Encoding.UTF8.GetBytes(noLayerSource));
        string cleared = Text(MochizukiShareCode.Apply(Encoding.UTF8.GetBytes(target), noLayerCode));
        Check(cleared.Contains("; [Layer1] retired") && cleared.Contains("; ToggleKey = F9"), "local layer archived");
        Check(!System.Text.RegularExpressions.Regex.IsMatch(cleared, @"(?m)^\[Layer1\]"), "global import leaves no active layer");
        Check(!cleared.Contains("NRIntensity=0.75") && !cleared.Contains("Passes=2"), "style values of old layers removed");

        string wilsjo = "[wilsjo2]\nHistory=0.5\nPreset=2\nPassesMode=fast\nClassicScaler=lanczos3\n" +
            "Downscale=bilinear\nFastMethod=feedback\nFastEditStrength=3.5\nColourLimit=off\n" +
            "FastFeedbackHaloLimit=yes\nPass2Preset=3\nPass30AutoMask=no\n";
        var wcode = MochizukiShareCode.Decode(MochizukiShareCode.Encode(Encoding.UTF8.GetBytes(wilsjo)));
        Check(wcode.Entries.Count == 11, "consumer wilsjo keys exported: " + wcode.Entries.Count);
        string reno = "[RenoDX]\nNRPresentGuides=2\nNRFastFeedbackHaloLimit=on\n";
        var rcode = MochizukiShareCode.Decode(MochizukiShareCode.Encode(Encoding.UTF8.GetBytes(reno)));
        Check(rcode.Entries.Count == 2, "renodx enum/bool exported");
        foreach (var value in new[] { "yes", "no", "on", "off" })
        {
            var d = MochizukiShareCode.Decode(MochizukiShareCode.Encode(Encoding.UTF8.GetBytes("[RenoDX]\nEnabled=" + value + "\n")));
            Check(d.Entries.Count == 1, "bool word " + value);
        }
        string dup = "[wilsjo2]\nHistory=0.5\nHistory=1.8\n[Layer1]\nNetwork=CC_SMALL_FP8\nNRIntensity=0.25\n" +
            "[Layer1]\nNetwork=CC_SILVER_AARDWOLD\n";
        var dupc = MochizukiShareCode.Decode(MochizukiShareCode.Encode(Encoding.UTF8.GetBytes(dup)));
        string dupHistory = "", dupNetwork = "";
        foreach (var e in dupc.Entries) {
            if (e.Key == "History") dupHistory = e.Value;
            if (e.Layer == 1 && e.Key == "Network") dupNetwork = e.Value;
        }
        Check(dupHistory == "1.8", "last duplicate key wins");
        Check(dupNetwork == "CC_SILVER_AARDWOLD", "last duplicate layer section wins");
        Check(dupc.Entries.Count == 2, "omitted key of replaced layer is not merged");
        foreach (var e in dupc.Entries) Check(!(e.Layer == 1 && e.Key == "NRIntensity"), "later layer section dropped old NRIntensity");
        string dupTarget = "[Layer1]\nToggleKey=F8\n; earlier local\n[Layer1]\nToggleKey=F9\n; last local\n";
        string globalCode = MochizukiShareCode.Encode(Encoding.UTF8.GetBytes("[Network]\nModel=CC_SMALL_FP8\n"));
        string dupApplied = Text(MochizukiShareCode.Apply(Encoding.UTF8.GetBytes(dupTarget), globalCode));
        Check(dupApplied.Contains("ToggleKey=F9") && !dupApplied.Contains("ToggleKey=F8"), "duplicate target layer keeps only last local section");
        Check(!System.Text.RegularExpressions.Regex.IsMatch(dupApplied, @"(?m)^\[Layer1\]"), "duplicate target layer archived inactive");
        foreach (var old in new[] { "MZ1:0=1;0000", "MZ2:0=1|pbz;0000", "MZ3:0=1|pbz|note;0000" })
        {
            try { MochizukiShareCode.Decode(old); throw new Exception("old code accepted"); }
            catch (MochizukiShareCode.CodeException ex) { Check(ex.Reason == "old", "old reason"); }
        }
        foreach (var note in new[] { "warm|night", "warm;night", "a~7Cb", "semi;and|pipe" })
        {
            string noteCode = MochizukiShareCode.Encode(Encoding.UTF8.GetBytes("[RenoDX]\nNRIntensity=1\n"), "pbz", note);
            var noteDecoded = MochizukiShareCode.Decode(noteCode);
            Check(noteDecoded.Note == note, "note roundtrip " + note + " -> " + noteDecoded.Note);
            string extracted = MochizukiShareCode.ExtractCode("chat text " + noteCode + " tail");
            Check(extracted == noteCode, "paste extraction " + note);
            Check(MochizukiShareCode.Decode(extracted).Note == note, "paste decode " + note);
        }
        string layerCase = MochizukiShareCode.Encode(Encoding.UTF8.GetBytes(
            "[Network]\nModel=CC_SMALL_FP8\n[Layer3]\nProcessing=RenoDX\nPlacement=before\nNetwork=CC_SILVER_AARDWOLD\n"));
        var layerDecoded = MochizukiShareCode.Decode(layerCase);
        string layerSummary = MochizukiShareCode.Summary(layerDecoded, true);
        Check(layerDecoded.LayerCount == 1 && layerDecoded.FirstLayer == 3, "actual layer set");
        Check(layerSummary.Contains("CC_SILVER_AARDWOLD") && layerSummary.Contains("1 个图层") && layerSummary.Contains("超分前"), "layer summary: " + layerSummary);
        Check(!layerSummary.Contains("3 个图层"), "no max-number layer count");
        string layerOffSource = "[Network]\nModel=CC_SMALL_FP8\n[NRControl]\nLayers=0\n[Layer1]\nProcessing=RenoDX\nPlacement=after\nNetwork=CC_SILVER_AARDWOLD\n[Layer3]\nProcessing=wilsjo2\nPlacement=before\n";
        string layerOff = MochizukiShareCode.Encode(Encoding.UTF8.GetBytes(layerOffSource));
        string layerOffSummary = MochizukiShareCode.Summary(MochizukiShareCode.Decode(layerOff), true);
        Check(layerOffSummary.Contains("CC_SILVER_AARDWOLD") && layerOffSummary.Contains("1 个图层") &&
              layerOffSummary.Contains("超分后") && layerOffSummary.Contains("1 层未启用"), "layers off summary: " + layerOffSummary);
        string layerOn = MochizukiShareCode.Encode(Encoding.UTF8.GetBytes(layerOffSource.Replace("Layers=0", "Layers=1")));
        string layerOnSummary = MochizukiShareCode.Summary(MochizukiShareCode.Decode(layerOn), true);
        Check(layerOnSummary.Contains("2 个图层") && layerOnSummary.Contains("超分后"), "layers on summary: " + layerOnSummary);        string disabledFirstSource = "[Network]\nModel=CC_SMALL_FP8\n[NRControl]\nLayers=0\n[Layer1]\nEnabled=0\nProcessing=RenoDX\nPlacement=after\nNetwork=CC_SILVER_AARDWOLD\n[Layer2]\nEnabled=1\nProcessing=wilsjo2\nPlacement=before\nNetwork=CC_SMALL_FP8_EXPERIMENTAL\n";
        string disabledFirst = MochizukiShareCode.Encode(Encoding.UTF8.GetBytes(disabledFirstSource));
        string disabledSummary = MochizukiShareCode.Summary(MochizukiShareCode.Decode(disabledFirst), true);
        Check(disabledSummary.Contains("0 个图层") && disabledSummary.Contains("不运行"), "disabled first layer not running: " + disabledSummary);
        Check(!disabledSummary.Contains("CC_SMALL_FP8_EXPERIMENTAL"), "disabled first was skipped: " + disabledSummary);
        string layersOnDisabled = MochizukiShareCode.Encode(Encoding.UTF8.GetBytes(disabledFirstSource.Replace("Layers=0", "Layers=1")));
        string layersOnDisabledSummary = MochizukiShareCode.Summary(MochizukiShareCode.Decode(layersOnDisabled), true);
        Check(layersOnDisabledSummary.Contains("1 个图层") && layersOnDisabledSummary.Contains("CC_SMALL_FP8_EXPERIMENTAL") &&
              layersOnDisabledSummary.Contains("1 层未启用"), "layers on disabled count: " + layersOnDisabledSummary);


        var empty = MochizukiShareCode.Decode(MochizukiShareCode.Encode(Encoding.UTF8.GetBytes(";default\n")));
        Check(empty.Entries.Count == 0, "empty code stays valid");
        var emptyResets = MochizukiShareCode.ChangedEntries(Encoding.UTF8.GetBytes("[RenoDX]\nNRIntensity=2\n"), empty);
        Check(emptyResets.Count == 1 && emptyResets[0].Reset && emptyResets[0].Key == "NRIntensity", "empty code reports reset");
        string tampered = code.Substring(0, code.Length - 1) + (code[code.Length - 1] == '0' ? '1' : '0');
        try { MochizukiShareCode.Decode(tampered); throw new Exception("crc not checked"); }
        catch (MochizukiShareCode.CodeException ex) { Check(ex.Reason == "crc", "crc reason"); }
        Console.WriteLine("PASS mz4 share code");
    }
}
