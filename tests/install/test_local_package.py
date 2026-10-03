import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / 'tools/release/BUILD_LOCAL_PACKAGE.ps1'
PS = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'


class LocalPackageTests(unittest.TestCase):
    def setUp(self):
        scratch = REPO / 'work/scratch'
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='package test ', dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        subprocess.run(['git', 'init', '-q', str(self.root)], check=True)
        files = {
            'VERSION': b'1.0.0\r\n',
            'tools/build/build-lmxxf-runtime.cmd': b'@exit /b 0\r\n',
            'tests/_lib/msvc-env.cmd': b'@exit /b 23\r\n',
            'tools/release/PACKAGE_RELEASE.ps1': b'throw "Must not package a failed build"',
            'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/OptiScaler.vcxproj': b'fixture',
        }
        for name, data in files.items():
            p = self.root / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
        for name in ['amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll',
                     'amd_fidelityfx_framegeneration_dx12.dll', 'amd_fidelityfx_vk.dll',
                     'libxess.dll', 'libxess_fg.dll', 'libxell.dll', 'D3D12_OptiScaler/D3D12Core.dll']:
            p = self.root / 'dist/previous/OptiScaler' / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(b'dependency fixture')

    def run_script(self, *args):
        return subprocess.run([str(PS), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                               str(SCRIPT), '-Root', str(self.root), *args],
                              capture_output=True, timeout=60)

    def test_plan_does_not_modify_version_or_build(self):
        result = self.run_script('-Version', '1.9.10.3', '-PlanOnly')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.root / 'VERSION').read_bytes(), b'1.0.0\r\n')
        self.assertFalse((self.root / 'exports').exists())

    def test_bad_version_rejected(self):
        result = self.run_script('-Version', '../bad', '-PlanOnly')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.root / 'VERSION').read_bytes(), b'1.0.0\r\n')

    def test_failure_stops_and_restores_version(self):
        result = self.run_script('-Version', '1.9.10.3')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'Compilation failed', result.stdout + result.stderr)
        self.assertEqual((self.root / 'VERSION').read_bytes(), b'1.0.0\r\n')
        self.assertEqual(list((self.root / 'exports').glob('*.zip')), [])
        self.assertEqual(list((self.root / 'dist').glob('*.zip')), [])

    def test_mochizuki_requires_builder(self):
        p = self.root / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrRuntime.cpp'
        p.parent.mkdir(parents=True)
        p.write_text('fixture')
        result = self.run_script('-Version', '1.9.10.3', '-PlanOnly')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'Mochizuki build script missing', result.stdout)


if __name__ == '__main__':
    unittest.main()
