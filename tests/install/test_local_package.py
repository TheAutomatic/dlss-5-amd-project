import os
import shutil
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

    def test_no_previous_package_uses_source_checkout(self):
        shutil.rmtree(self.root / 'dist')
        result = self.run_script('-Version', '1.12.1', '-PlanOnly')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(b'using dependency binaries from the source checkout', result.stdout)
        self.assertFalse((self.root / 'exports').exists())
        result = self.run_script('-Version', '1.12.1')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'Compilation failed', result.stdout + result.stderr)
        self.assertEqual((self.root / 'VERSION').read_bytes(), b'1.0.0\r\n')

    def test_bad_version_rejected(self):
        result = self.run_script('-Version', '../bad', '-PlanOnly')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.root / 'VERSION').read_bytes(), b'1.0.0\r\n')

    def test_prerelease_plan(self):
        result = self.run_script('-Version', '1.10.5-alpha', '-PlanOnly')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.root / 'exports').exists())

    def test_failure_stops_and_restores_version(self):
        result = self.run_script('-Version', '1.9.10.3')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'Compilation failed', result.stdout + result.stderr)
        self.assertEqual((self.root / 'VERSION').read_bytes(), b'1.0.0\r\n')
        self.assertEqual(list((self.root / 'exports').glob('*.zip')), [])
        self.assertEqual(list((self.root / 'dist').glob('*.zip')), [])

    def test_generated_batch_selects_incremental_and_forced_build(self):
        for flags, target in (((), 'Build'), (('-Rebuild',), 'Rebuild')):
            result = self.run_script('-Version', '1.9.10.3', *flags)
            self.assertNotEqual(result.returncode, 0)  # Fixture intentionally fails compiler setup.
            batch = max((self.root / 'work/scratch').glob('local-package-*/build.cmd'), key=lambda p: p.stat().st_mtime)
            self.assertIn('/t:' + target + ' ', batch.read_text())
            self.assertNotIn('@HOST_TARGET@', batch.read_text())

    def test_crt_discovery_skips_incomplete_redist(self):
        source = (REPO / 'tools/release/prepare-person-assets.ps1').read_text(encoding='utf-8-sig')
        selection = source[source.index('$crtNames='):source.index('Get-ChildItem -LiteralPath $CrtDir -File')]
        vc = self.root / 'VC'
        complete = vc / 'Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT'
        complete.mkdir(parents=True)
        for name in ('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'):
            (complete / name).write_bytes(b'fixture')
        incomplete = vc / 'Redist/MSVC/14.51.36231'
        incomplete.mkdir()
        script = self.root / 'crt.ps1'
        script.write_text("$ErrorActionPreference='Stop'\n$CrtDir=''\n" + selection,
                          encoding='utf-8-sig')
        env = dict(os.environ, VCINSTALLDIR=str(vc), VCToolsRedistDir=str(incomplete))
        def run():
            return subprocess.run([str(PS), '-NoProfile', '-File', str(script)],
                                  env=env, capture_output=True, timeout=20)
        result = run()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(b'14.44.35112', result.stdout)
        (complete / 'vcruntime140_1.dll').unlink()
        result = run()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'No complete MSVC x64', result.stdout + result.stderr)

    def test_native_stderr_uses_exit_code_and_keeps_log(self):
        # Run the actual helper with Windows PowerShell 5.1; a source-text check would miss this bug.
        source = SCRIPT.read_text(encoding='utf-8-sig')
        helper = source[source.index('function Invoke-LoggedNative'):source.index('\ntry {')]
        for code in (0, 23):
            log = self.root / f'native-{code}.log'
            fixture = self.root / f'native-{code}.ps1'
            fixture.write_text("$ErrorActionPreference = 'Stop'\n" + helper +
                "\ntry { Invoke-LoggedNative -Executable $env:ComSpec "
                f"-Arguments @('/d','/c','echo compiler-warning 1>&2 & exit /b {code}') "
                f"-Log '{log}' -Failure 'Native failed'; exit 0 " +
                "} catch { Write-Host $_.Exception.Message; exit 1 }", encoding='utf-8-sig')
            result = subprocess.run([str(PS), '-NoProfile', '-File', str(fixture)], capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0 if code == 0 else 1, result.stdout + result.stderr)
            self.assertIn('compiler-warning', log.read_text(encoding='utf-16'))
            if code:
                self.assertIn(b'exit 23', result.stdout)


if __name__ == '__main__':
    unittest.main()
