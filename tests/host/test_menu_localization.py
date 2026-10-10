"""Catalog and persistence checks. Rendering/ID/disabled/glyph tests use real ImGui in the paired C++ test."""
from pathlib import Path
import json
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
PRODUCT = ROOT / 'OptiScaler-DLSSNR-PreSR-Multipass-main'
CODE = PRODUCT / 'OptiScaler'
LITERAL = r'"(?:\\.|[^"\\])*"'
CATALOG = (CODE / 'menu/MenuStrings.inl').read_text(encoding='utf-8')
ENTRIES = [(json.loads(a), json.loads(b)) for a, b in re.findall(r'\{(' + LITERAL + r'),\s*(' + LITERAL + r')\}', CATALOG)]
FORMATS = re.compile(r'%[-+#0]*\d*(?:\.\d+)?(?:hh|ll|I64|h|l|z|t|L)?[diuoxXfFeEgGaAcspn%]')

class MenuLocalizationTests(unittest.TestCase):
    def test_no_duplicate_keys_or_changed_printf_contract(self):
        self.assertEqual(len(dict(ENTRIES)), len(ENTRIES))
        for english, chinese in ENTRIES:
            with self.subTest(english=english):
                self.assertTrue(chinese)
                self.assertNotIn('\ufffd', chinese)
                self.assertEqual(FORMATS.findall(english), FORMATS.findall(chinese))
                self.assertNotIn('%n', chinese)

    def test_mochizuki_metadata_and_core_labels_translated(self):
        labels = re.findall(LITERAL, (CODE / 'dlssnr/backend/MochizukiOptions.inc').read_text(encoding='utf-8'))
        for literal in labels:
            self.assertIn(json.loads(literal), dict(ENTRIES))
        for text in ('Language', 'Input', 'Model', 'Output', 'Prepare NR input', 'NR model', 'Apply NR edit',
                     'Apply NR edit (experimental)',
                     'Temporal history', 'Temporal history (anti-flicker)', 'ViT adaptive reuse', 'Save Settings', 'Close',
                     'Low-frequency gain', 'Fine-detail gain', 'Skin detail protection', 'Edge detail protection',
                     'Person protection', 'Person NR strength', 'Person detail'):
            self.assertIn(text, dict(ENTRIES))

    def test_language_is_host_owned_and_default_english(self):
        config = (CODE / 'Config.cpp').read_text(encoding='utf-8')
        header = (CODE / 'Config.h').read_text(encoding='utf-8')
        keys = (CODE / 'ConfigKeys.h').read_text(encoding='utf-8')
        ini = (PRODUCT / 'OptiScaler.ini').read_text(encoding='utf-8')
        self.assertIn('MenuLanguage { "en" }', header)
        self.assertIn('MenuLanguage = "Language"', keys)
        self.assertIn('MenuLanguage,', keys.split('kKnown[]', 1)[1])
        self.assertIn('readString(CfgKey::kMenuSection, CfgKey::MenuLanguage, true)', config)
        self.assertIn('ini.SetValue(CfgKey::kMenuSection, CfgKey::MenuLanguage,', config)
        self.assertNotRegex(config, r'PutEnv\w*\(CfgKey::MenuLanguage')
        self.assertIn('Language=en', ini.split('[Menu]', 1)[1].split('\n[', 1)[0])
        runtime = (CODE / 'dlssnr/backend/lmxxf_runtime/LmxxfNrRuntime.cpp').read_text(encoding='utf-8')
        self.assertNotIn('MenuLocale', runtime)

if __name__ == '__main__':
    unittest.main(verbosity=2)
