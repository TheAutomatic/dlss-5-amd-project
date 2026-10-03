"""Exercise the real test entrypoint's artifact proof with isolated suite stubs."""
import hashlib
import os
from pathlib import Path
import runpy
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
        for area in ('host', 'shader', 'lmxxf', 'mochizuki', 'install', 'sync'):
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
        # Exercise the real source/artifact gate. Only suite execution is stubbed;
        # this fixture tests the entrypoint's proof orchestration, not GPU code.
        for directory in ('third_party/mochizuki/windows',
                          'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/mochizuki_runtime'):
            shutil.copytree(ROOT / directory, self.root / directory,
                            ignore=shutil.ignore_patterns('__pycache__'))
        for name in ('tools/build/mochizuki-manifest.py', 'tools/build/build-mochizuki-runtime.py',
                     'tools/build/build-mochizuki-runtime.cmd', 'tools/build/mochizuki-deps.py',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/NrPerformance.h'):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, target)
        self.mochizuki = self.root / 'exports/mochizuki-runtime'
        (self.mochizuki / 'dlssnr-amd/shaders').mkdir(parents=True)
        (self.mochizuki / 'MochizukiNrRuntime.dll').write_bytes(b'non-executable Mochizuki fixture')
        (self.mochizuki / 'dlssnr-amd/shaders/test.spv').write_bytes(b'non-executable shader fixture')
        runpy.run_path(str(self.root / 'tools/build/mochizuki-manifest.py'))['write'](self.mochizuki)

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

    def test_mochizuki_mismatch_invalidates_previous_proof(self):
        (self.mochizuki / 'MochizukiNrRuntime.dll').write_bytes(b'changed fixture')
        result = self.run_entrypoint()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(b'Mochizuki source/artifact mismatch', result.stdout + result.stderr)
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
