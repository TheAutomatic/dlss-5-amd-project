"""Contract tests: Ins/ini win over native-game-flags.txt; key names stay stable."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
KEYS = (ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/ConfigKeys.h").read_text(encoding="utf-8")
CONFIG = (ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/Config.cpp").read_text(encoding="utf-8")
RUNTIME = (
    ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrRuntime.cpp"
).read_text(encoding="utf-8")
MENU = (ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/DlssNr_Menu.cpp").read_text(encoding="utf-8")


class ConfigPriorityTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt" and shutil.which("cl"), "requires an x64 MSVC developer environment")
    def test_host_updates_cross_independent_crt_caches(self):
        source = ROOT / "tests/host/lmxxf_config_crt.cpp"
        with tempfile.TemporaryDirectory(prefix="lmxxf-config-crt-") as td:
            out = Path(td)
            for fixture in (True, False):
                name = "runtime" if fixture else "host"
                target = out / (name + (".dll" if fixture else ".exe"))
                command = ["cl", "/nologo", "/std:c++17", "/EHsc", "/MT", "/W4", "/utf-8",
                           "/D_CRT_SECURE_NO_WARNINGS", str(source),
                           f"/Fo:{out / (name + '.obj')}", f"/Fe:{target}"]
                if fixture:
                    command += ["/LD", "/DLMXXF_CONFIG_CRT_FIXTURE"]
                    command += [f"/I{ROOT / 'third_party/lmxxf/Development/HIP'}",
                                f"/I{ROOT / 'third_party/lmxxf/src'}", "/DNOMINMAX"]
                result = subprocess.run(command, cwd=out, capture_output=True, text=True,
                                        errors="replace", timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(out / "host.exe"), str(out / "runtime.dll")],
                                    capture_output=True, text=True, errors="replace", timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("LMXXF_CONFIG_CRT_OK", result.stdout)

    def test_cross_layer_keys_use_upstream_dlss5_names(self):
        for name, ini_key in re.findall(r'inline constexpr const char \*(\w+) = "([^"]+)"', KEYS):
            if name.startswith("Mochizuki"):
                # Independent C-struct controls, not HIP environment variables.
                self.assertEqual(name, ini_key)
                self.assertIn(f"CfgKey::{name}", CONFIG)
                self.assertIn(f"    {name},", KEYS)
                continue
            if name.endswith("Legacy"):
                self.assertFalse(ini_key.startswith("DLSS5_"), name)
                continue
            if name in {
                "AutoExposure",
                "AutoExposureScale",
                "PaperWhite",
                "AllowEnhancedBarriers",
                "EarlyExeWrap",
                "Diagnostic",
                "NrTimingEnabled",  # Product telemetry, not an upstream kernel option.
                "NrTimingLog",
                "LmxxfModelHistory",  # Host-owned FrameInfo flag, not an upstream environment key.
                "NrStabilizerEnabled", "NrStabilizerAlpha", "NrStabilizerThreshold",
                "NrOverallIntensity",  # Shared final-output blend, no runtime environment alias.
                "kSection",
                "kDanielSection",
                "kMenuSection",  # Host window settings do not cross the runtime boundary.
                "MenuLanguage", "MenuWindowWidth", "MenuWindowHeight", "MenuWindowAnchor",
                "ToneCurve",
                "ToneLift",
                "AmdUseGameExposure",
                "AmdToneChannels",
                "Quality",
                "XeFGInterpolationCount",
                "QueuePriority",
                "QueuePriorityLegacy",
                "Inline",
                "Async",
            }:
                continue
            self.assertTrue(ini_key.startswith("DLSS5_"), f"{name} -> {ini_key}")

    def test_env_alias_is_identity_for_unified_keys(self):
        self.assertIn("if (std::strncmp(iniKey, \"DLSS5_\", 6) == 0)", KEYS)
        self.assertNotIn('return "DLSS5_FIT_LARGE"', KEYS)

    def test_nr_multiplier_has_separate_persistent_key(self):
        self.assertIn('XeFGInterpolationCount = "XeFGInterpolationCount"', KEYS)
        self.assertIn('    XeFGInterpolationCount,', KEYS.split('kKnown[]', 1)[1])
        self.assertIn('DlssNrXeFGInterpolationCount.set_from_config(readInt(CfgKey::kSection, CfgKey::XeFGInterpolationCount))', CONFIG)
        self.assertIn('ini.SetValue(CfgKey::kSection, CfgKey::XeFGInterpolationCount,', CONFIG)
        self.assertIn('Instance()->DlssNrXeFGInterpolationCount.value_for_config()', CONFIG)
        self.assertIn('DlssNrXeFGInterpolationCount.value() < 0', CONFIG)
        xefg = (ROOT / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/framegen/xefg/XeFG_Dx12.cpp').read_text(encoding='utf-8')
        self.assertNotRegex(xefg, r'FGXeFGInterpolationCount\s*=')
        self.assertNotIn('FGXeFGInterpolationCount.set_volatile_value', xefg)
        self.assertIn('DlssNr::IsActiveForFrameGeneration()', xefg)
        nr = (ROOT / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp').read_text(encoding='utf-8')
        activity = nr.split('bool IsActiveForFrameGeneration()', 1)[1].split('const char* FailureReason()', 1)[0]
        self.assertIn('g_activityBackend.load', activity)
        self.assertNotIn('HasFiles()', activity)

    def test_flags_fallback_does_not_overwrite_env(self):
        self.assertIn("if (std::getenv(key))", RUNTIME)
        self.assertIn("continue; // menu / ini / caller already owns this key", RUNTIME)
        self.assertIn("ApplyFlagsFileFallback", RUNTIME)

    def test_host_writes_env_from_ini_before_flags(self):
        self.assertIn("PutEnvAlias(CfgKey::FitLarge", CONFIG)
        self.assertIn("PutEnvAlias(CfgKey::Pdl", CONFIG)
        self.assertIn("PutEnvString(CfgKey::NetworkHeight", CONFIG)
        self.assertIn("PutEnvString(CfgKey::SkipBlocks", CONFIG)
        self.assertIn("Instance()->LmxxfSkipBlocks.value_for_config_or", CONFIG)
        # Legacy names are read, never written back as the save key.
        self.assertIn("FitLargeLegacy", CONFIG)
        self.assertNotIn('SetValue(CfgKey::kSection, CfgKey::FitLargeLegacy', CONFIG)

    def test_menu_labels_are_not_ini_keys(self):
        # Display strings must not appear as SetValue / CfgKey string literals.
        for label in ("High resolution", "Network tier", "Wave-owned attention", "ViT adaptive reuse"):
            self.assertNotIn(f'"{label}"', CONFIG)
            self.assertNotIn(f'"{label}"', KEYS)



if __name__ == "__main__":
    unittest.main(verbosity=2)
