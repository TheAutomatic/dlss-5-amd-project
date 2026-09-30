"""Exercise the real test entrypoint's artifact proof with isolated suite stubs."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == 'nt', 'Windows batch entrypoint')
class RuntimeProofTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='lmxxf runtime proof ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'tests/_lib').mkdir(parents=True)
        shutil.copy2(ROOT / 'tests/run-all.cmd', self.root / 'tests/run-all.cmd')
        (self.root / 'tests/_lib/msvc-env.cmd').write_text('@exit /b 0\n')
        for area in ('host', 'shader', 'lmxxf', 'install', 'sync'):
            (self.root / 'tests' / area).mkdir()
            (self.root / 'tests' / area / 'run.cmd').write_text('@exit /b 0\n')
        self.runtime = self.root / 'fixture runtime.dll'
        self.runtime.write_bytes(b'non-executable runtime fixture')
        self.out = self.root / 'exports/proof'
        self.out.mkdir(parents=True)
        self.proof = self.out / 'runtime-ci.sha256'
        self.proof.write_text('old proof')
        self.env = {key.upper(): value for key, value in os.environ.items()}
        self.env['LMXXF_TEST_RUNTIME'] = str(self.runtime)

    def run_entrypoint(self, *args):
        return subprocess.run(
            ['cmd', '/d', '/c', 'tests\\run-all.cmd', '--tier', 'ci',
             '--out', 'exports\\proof', *args],
            cwd=self.root, env=self.env, capture_output=True, timeout=45)

    def test_success_binds_exact_tested_bytes(self):
        result = self.run_entrypoint()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.proof.read_text().strip().lower(),
                         hashlib.sha256(self.runtime.read_bytes()).hexdigest())

    def test_failure_invalidates_previous_proof(self):
        (self.root / 'tests/sync/run.cmd').write_text('@exit /b 7\n')
        result = self.run_entrypoint()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.proof.exists())

    def test_skip_sync_cannot_produce_release_proof(self):
        result = self.run_entrypoint('--skip-sync')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.proof.exists())

    def test_replacing_runtime_during_suite_invalidates_proof(self):
        (self.root / 'tests/sync/run.cmd').write_text(
            '@echo changed>"%LMXXF_TEST_RUNTIME%"\n@exit /b 0\n')
        result = self.run_entrypoint()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse(self.proof.exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
