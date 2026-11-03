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
            "[Network]\nModel = CC_SMALL_FP8\nStyle Grade = 1\nHotkey = Ctrl+F11\nResidency = auto\n" +
            "[Preprocess]\nEnabled = 1\nExposure = auto\nExposureBias = -0.25\nCurve = filmic\n" +
            "[RenoDX]\nEnabled = 1\nNRIntensity = 1.25\nNRPassesMode = legacy\nNRChrome = 0\n" +
            "[wilsjo2]\nPasses = auto\nStyle = 2\nRunBeforeSR = true\nAutoExposureTrimAnchors = 0.0:1.0\n" +
            "[HudFix]\nMode = soft\nShowMask = false\n" +
            "[NRControl]\nLayers = 1\nMenuKey = End\n" +
            "[Layer1]\nEnabled = 1\nProcessing = RenoDX\nPlacement = present\nNetwork = CC_SMALL_FP8_EXPERIMENTAL\nNRIntensity = 0.75\n" +
            "[Layer2]\nEnabled = 0\nProcessing = wilsjo2\nPlacement = pre_sr\nNetwork = CC_SILVER_AARDWOLD\nPasses = 2\n";
        string code = MochizukiShareCode.Encode(Encoding.UTF8.GetBytes(source), "pbz.exe", "night look");
        Check(code.StartsWith("MZ4:", StringComparison.Ordinal), "prefix");
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
        Check(sawAuto, "auto preserved");
        Check(sawRunBefore, "bool true preserved");
        Check(sawAnchors, "anchor string preserved");

        string target =
            "[Network]\nModel = CC_SILVER_AARDWOLD\nHotkey = Ctrl+F12\n" +
            "[Layer3]\nEnabled = 1\nProcessing = RenoDX\nNetwork = CC_SMALL_FP8\n" +
            "[Inspect]\nCompare = 1\n" +
            "[Log]\nEnabled = 1\n";
        string applied = Text(MochizukiShareCode.Apply(Encoding.UTF8.GetBytes(target), code));
        Check(applied.Contains("[Layer1]") && applied.Contains("[Layer2]"), "layers written");
        Check(!applied.Contains("[Layer3]"), "stale layer removed");
        Check(applied.Contains("Model=CC_SMALL_FP8"), "global model applied");
        Check(applied.Contains("Hotkey = Ctrl+F12"), "unrelated key preserved");
        Check(applied.Contains("[Inspect]") && applied.Contains("Compare = 1"), "inspect preserved");
        Check(applied.Contains("Passes=auto"), "auto written");
        Check(applied.Contains("NRChrome = 0") == false, "unknown source key not exported");

        string noLayerSource = "[Network]\nModel = CC_SILVER_AARDWOLD\n[wilsjo2]\nPasses = 1\n";
        string noLayerCode = MochizukiShareCode.Encode(Encoding.UTF8.GetBytes(noLayerSource));
        string cleared = Text(MochizukiShareCode.Apply(Encoding.UTF8.GetBytes(target), noLayerCode));
        Check(!cleared.Contains("[Layer1]") && !cleared.Contains("[Layer3]"), "global code clears layers");

        foreach (var old in new[] { "MZ1:0=1;0000", "MZ2:0=1|pbz;0000", "MZ3:0=1|pbz|note;0000" })
        {
            try { MochizukiShareCode.Decode(old); throw new Exception("old code accepted"); }
            catch (MochizukiShareCode.CodeException ex) { Check(ex.Reason == "old", "old reason"); }
        }
        string tampered = code.Substring(0, code.Length - 1) + (code[code.Length - 1] == '0' ? '1' : '0');
        try { MochizukiShareCode.Decode(tampered); throw new Exception("crc not checked"); }
        catch (MochizukiShareCode.CodeException ex) { Check(ex.Reason == "crc", "crc reason"); }
        Console.WriteLine("PASS mz4 share code");
    }
}
