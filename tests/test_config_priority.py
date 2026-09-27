"""Contract tests: Ins/ini win over native-game-flags.txt; key names stay stable."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
KEYS = (ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/ConfigKeys.h").read_text(encoding="utf-8")
CONFIG = (ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/Config.cpp").read_text(encoding="utf-8")
RUNTIME = (
    ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrRuntime.cpp"
).read_text(encoding="utf-8")
MENU = (ROOT / "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/DlssNr_Menu.cpp").read_text(encoding="utf-8")


class ConfigPriorityTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt" and shutil.which("cl"), "requires an x64 MSVC developer environment")
    def test_host_updates_cross_independent_crt_caches(self):
        source = ROOT / "tests/lmxxf_config_crt.cpp"
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
                result = subprocess.run(command, cwd=out, capture_output=True, text=True,
                                        errors="replace", timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(out / "host.exe"), str(out / "runtime.dll")],
                                    capture_output=True, text=True, errors="replace", timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("LMXXF_CONFIG_CRT_OK", result.stdout)

    def test_cross_layer_keys_use_upstream_dlss5_names(self):
        for name, ini_key in re.findall(r'inline constexpr const char \*(\w+) = "([^"]+)"', KEYS):
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
                "kSection",
                "kDanielSection",
                "ToneCurve",
                "ToneLift",
                "Quality",
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

    def test_flags_fallback_does_not_overwrite_env(self):
        self.assertIn("if (std::getenv(key))", RUNTIME)
        self.assertIn("continue; // menu / ini / caller already owns this key", RUNTIME)
        self.assertIn("ApplyFlagsFileFallback", RUNTIME)

    def test_host_writes_env_from_ini_before_flags(self):
        self.assertIn("PutEnvAlias(CfgKey::FitLarge", CONFIG)
        self.assertIn("PutEnvAlias(CfgKey::Pdl", CONFIG)
        self.assertIn("PutEnvString(CfgKey::NetworkHeight", CONFIG)
        # Legacy names are read, never written back as the save key.
        self.assertIn("FitLargeLegacy", CONFIG)
        self.assertNotIn('SetValue(CfgKey::kSection, CfgKey::FitLargeLegacy', CONFIG)

    def test_menu_labels_are_not_ini_keys(self):
        # Display strings must not appear as SetValue / CfgKey string literals.
        for label in ("High resolution", "Network tier", "Wave-owned attention", "ViT adaptive reuse"):
            self.assertNotIn(f'"{label}"', CONFIG)
            self.assertNotIn(f'"{label}"', KEYS)

    def test_priority_simulation_ini_beats_txt(self):
        # Same rule as ApplyFlagsFileFallback: fill only missing keys.
        env = {"DLSS5_FIT_LARGE": "1"}  # ini/menu already wrote this
        flags = {"DLSS5_FIT_LARGE": "0", "DLSS5_HIP_WAVE_OWNED": "1"}
        for key, value in flags.items():
            if key in env:
                continue
            env[key] = value
        self.assertEqual(env["DLSS5_FIT_LARGE"], "1")
        self.assertEqual(env["DLSS5_HIP_WAVE_OWNED"], "1")


if __name__ == "__main__":
    unittest.main(verbosity=2)
