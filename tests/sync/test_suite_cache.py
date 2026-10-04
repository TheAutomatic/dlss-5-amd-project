"""A prior pass is reusable; a miss, failure or changed input never creates proof."""
from datetime import datetime, timezone
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('cache', ROOT / 'tools/lmxxf-sync/test-cache.py')
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)


class CacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.receipt = self.root / 'exports/test-cache/sync.json'

    def seed(self, key='same'):
        self.receipt.parent.mkdir(parents=True, exist_ok=True)
        self.receipt.write_text(json.dumps({'schema': 1, 'key': key, 'result': 'passed'}))

    def test_identical_success_reuses_without_launch(self):
        self.seed()
        with patch.object(cache, 'fingerprint', return_value='same'), patch.object(cache.subprocess, 'call') as call:
            self.assertEqual(cache.run(self.root), 0)
            call.assert_not_called()

    def test_missing_stale_and_corrupt_receipts_run(self):
        for old in (None, 'stale', '{broken'):
            with self.subTest(old=old):
                self.receipt.unlink(missing_ok=True)
                if old:
                    self.receipt.parent.mkdir(parents=True, exist_ok=True)
                    self.receipt.write_text(old)
                with patch.object(cache, 'fingerprint', return_value='new'), patch.object(cache.subprocess, 'call', return_value=0) as call:
                    self.assertEqual(cache.run(self.root), 0)
                    call.assert_called_once()
                    self.assertTrue(cache.valid(self.receipt, 'new'))

    def test_failure_and_midrun_change_remove_old_proof(self):
        for code, keys in ((9, ['new']), (0, ['new', 'changed'])):
            with self.subTest(code=code):
                self.seed()
                with patch.object(cache, 'fingerprint', side_effect=keys), patch.object(cache.subprocess, 'call', return_value=code):
                    self.assertNotEqual(cache.run(self.root), 0)
                    self.assertFalse(self.receipt.exists())

    def test_force_reruns_even_matching_success(self):
        self.seed()
        with patch.object(cache, 'fingerprint', return_value='same'), patch.object(cache.subprocess, 'call', return_value=0) as call:
            self.assertEqual(cache.run(self.root, force=True), 0)
            call.assert_called_once()

    def test_content_addition_deletion_environment_and_week_invalidate(self):
        folder = self.root / 'inputs'; folder.mkdir()
        source = folder / 'input.txt'; source.write_text('before')
        now = datetime(2026, 10, 5, tzinfo=timezone.utc)
        with patch.object(cache, 'INPUTS', ('inputs',)), patch.object(cache.subprocess, 'check_output', return_value='v1'):
            old = cache.fingerprint(self.root, now)
            source.write_text('after'); self.assertNotEqual(old, cache.fingerprint(self.root, now))
            source.write_text('before'); added = folder / 'added.h'; added.write_text('new')
            self.assertNotEqual(old, cache.fingerprint(self.root, now)); added.unlink()
            self.assertEqual(old, cache.fingerprint(self.root, now))
            self.assertNotEqual(old, cache.fingerprint(self.root, datetime(2026, 10, 12, tzinfo=timezone.utc)))
            with patch.object(cache.subprocess, 'check_output', return_value='v2'):
                self.assertNotEqual(old, cache.fingerprint(self.root, now))
            source.unlink(); self.assertNotEqual(old, cache.fingerprint(self.root, now))


if __name__ == '__main__':
    unittest.main()
