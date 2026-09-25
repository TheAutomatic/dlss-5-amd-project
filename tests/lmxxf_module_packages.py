"""Run the real packaging/staging entrypoints with tiny, non-executable inputs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

from lmxxf_fixtures import damage_modules, make_modules, snapshot_files


REPO = Path(__file__).resolve().parents[1]
PS = os.environ.get('LMXXF_TEST_POWERSHELL', shutil.which('powershell.exe'))


@unittest.skipUnless(os.name == 'nt' and PS, 'Windows PowerShell required')
class ModulePackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='lmxxf package entrypoints ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'tools').mkdir()
        for name in ('PACKAGE_RELEASE.ps1', 'install-amd-presr.ps1', 'uninstall-amd-presr.ps1',
                     'lmxxf-module-package.ps1', 'stage-lmxxf-beside-optiscaler.cmd',
                     'stage-lmxxf-beside-optiscaler.ps1'):
            shutil.copy2(REPO / 'tools' / name, self.root / 'tools' / name)
        for name in ('README.md', 'README.en.md', 'README.es.md'):
            shutil.copy2(REPO / name, self.root / name)
        (self.root / 'OptiScaler-DLSSNR-PreSR-Multipass-main').mkdir()
        shutil.copy2(REPO / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler.ini',
                     self.root / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler.ini')
        self.opti = self.root / 'fixture-opti.dll'
        self.opti.write_bytes(b'fixture proxy, not executable')
        runtime = self.root / 'exports/lmxxf-runtime/LmxxfNrRuntime.dll'
        runtime.parent.mkdir(parents=True)
        runtime.write_bytes(b'fixture runtime, not executable')
        self.modules = make_modules(self.root / 'third_party/lmxxf/modules')
        shaders = self.root / 'third_party/lmxxf/shaders'
        shaders.mkdir()
        (shaders / 'native_codec_encode.hlsl').write_text('// fixture', encoding='utf-8')
        self.env = {k.upper(): v for k, v in os.environ.items()}
        self.env.pop('PSMODULEPATH', None)
        self.archive = self.root / 'dist/package.zip'

    def run_ps(self, args, env=None):
        result = subprocess.run([PS, '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', *args],
                                cwd=self.root, env=env or self.env, capture_output=True, timeout=60)
        return result.returncode, (result.stdout + result.stderr).decode('utf-8', errors='replace')

    def package(self):
        return self.run_ps(['-File', str(self.root / 'tools/PACKAGE_RELEASE.ps1'),
                           '-OptiDll', str(self.opti), '-AllowMissingDeps', '-Name', 'package'])

    def assert_rejected(self, fault):
        damage_modules(self.modules, fault)
        code, out = self.package()
        self.assertNotEqual(code, 0, out)
        self.assertFalse(self.archive.exists(), out)
        return out

    def test_valid_actual_archive_can_install(self):
        code, out = self.package()
        self.assertEqual(code, 0, out)
        extracted = self.root / 'extracted'
        with zipfile.ZipFile(self.archive) as archive:
            entries = archive.namelist()
            self.assertEqual(sum(p.endswith('.hsaco') for p in entries), 48)
            self.assertIn('lmxxf-module-package.ps1', entries)
            archive.extractall(extracted)
        game = self.root / 'game'
        game.mkdir()
        code, out = self.run_ps(['-File', str(extracted / 'Setup.ps1'), '-GameDir', str(game), '-NonInteractive'])
        self.assertEqual(code, 0, out)
        self.assertIn('Install SUCCEEDED', out)
        self.assertEqual(snapshot_files(game / 'lmxxf-modules'), snapshot_files(self.modules))
        code, out = self.run_ps(['-File', str(extracted / 'Setup.ps1'), '-GameDir', str(game),
                                 '-NonInteractive', '-UninstallExisting'])
        self.assertEqual(code, 0, out)
        self.assertIn('Uninstall SUCCEEDED', out)
        self.assertIn('Install SUCCEEDED', out)
        self.assertEqual(snapshot_files(game / 'lmxxf-modules'), snapshot_files(self.modules))

    def test_corrupt_module_never_produces_zip(self):
        self.assertIn('checksum mismatch', self.assert_rejected('corrupt'))

    def test_missing_architecture_never_produces_zip(self):
        self.assert_rejected('missing-arch')

    def test_parent_leaf_mismatch_never_produces_zip(self):
        self.assert_rejected('leaf-mismatch')

    def test_renamed_same_count_set_never_produces_zip(self):
        self.assert_rejected('rename')

    def test_missing_runtime_metadata_never_produces_zip(self):
        self.assert_rejected('missing-runtime')

    def test_incomplete_root_manifest_never_produces_zip(self):
        self.assert_rejected('incomplete-root')

    def test_duplicate_root_entry_never_produces_zip(self):
        self.assert_rejected('duplicate')

    def test_flat_module_never_produces_zip(self):
        self.assert_rejected('flat')

    def test_generated_source_is_rejected_by_actual_packager(self):
        (self.modules / 'gfx1200/kernel.generated.hip').write_bytes(b'build intermediate')
        code, out = self.package()
        self.assertNotEqual(code, 0, out)
        self.assertIn('Refusing to package', out)
        self.assertFalse(self.archive.exists())

    def test_assembly_is_rejected_by_actual_packager(self):
        (self.modules / 'gfx1201/kernel.hsaco.s').write_bytes(b'build intermediate')
        code, out = self.package()
        self.assertNotEqual(code, 0, out)
        self.assertIn('Refusing to package', out)
        self.assertFalse(self.archive.exists())

    def test_compressed_module_bytes_are_checked(self):
        # Run the unchanged entrypoint with a fault in the actual archive operation.
        # The original cmdlet still creates the ZIP, then one module byte is changed.
        wrapper = r'''
function Compress-Archive {
    param($Path, $DestinationPath, $CompressionLevel, [switch]$Force)
    Microsoft.PowerShell.Archive\Compress-Archive @PSBoundParameters
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::Open($DestinationPath, [IO.Compression.ZipArchiveMode]::Update)
    try {
        $entry = $zip.Entries | Where-Object { $_.FullName.Replace('\', '/') -eq 'lmxxf-modules/gfx1200/c32_fast.hsaco' } | Select-Object -First 1
        $stream = $entry.Open()
        try { $stream.WriteByte(42) } finally { $stream.Dispose() }
    } finally { $zip.Dispose() }
}
& $env:LMXXF_PACKAGE_ENTRY -OptiDll $env:LMXXF_PACKAGE_DLL -AllowMissingDeps -Name package
'''
        env = self.env.copy()
        env['LMXXF_PACKAGE_ENTRY'] = str(self.root / 'tools/PACKAGE_RELEASE.ps1')
        env['LMXXF_PACKAGE_DLL'] = str(self.opti)
        code, out = self.run_ps(['-Command', wrapper], env)
        self.assertNotEqual(code, 0, out)
        self.assertIn('Archive checksum mismatch', out)
        self.assertFalse(self.archive.exists(), 'invalid artifact must be removed')

    def make_arch_junction(self):
        linked = self.modules / 'gfx1200'
        outside = self.root / 'outside-modules'
        self.assertEqual(linked.resolve().parent, self.modules.resolve())
        linked.rename(outside)
        env = self.env.copy()
        env['LMXXF_TEST_LINK'] = str(linked)
        env['LMXXF_TEST_TARGET'] = str(outside)
        code, out = self.run_ps(['-Command', 'New-Item -ItemType Junction -Path $env:LMXXF_TEST_LINK -Target $env:LMXXF_TEST_TARGET | Out-Null'], env)
        self.assertEqual(code, 0, out)
        self.addCleanup(os.rmdir, linked)  # remove only the junction, before temp cleanup
        return outside

    def test_junction_bundle_rejected_without_touching_target(self):
        outside = self.make_arch_junction()
        before = snapshot_files(outside)
        code, out = self.package()
        self.assertNotEqual(code, 0, out)
        self.assertIn('linked module path', out)
        self.assertFalse(self.archive.exists())
        self.assertEqual(snapshot_files(outside), before)

    def stage(self, destination):
        # Exercise the .cmd entrypoint as well as its production PowerShell body.
        command = subprocess.list2cmdline([str(self.root / 'tools/stage-lmxxf-beside-optiscaler.cmd'), str(destination)])
        result = subprocess.run('cmd.exe /d /s /c "' + command + '"',
                                cwd=self.root, env=self.env, capture_output=True, timeout=60)
        return result.returncode, (result.stdout + result.stderr).decode('utf-8', errors='replace')

    def test_developer_staging_refuses_legacy_before_runtime_copy(self):
        game = self.root / 'legacy game'
        shutil.copytree(self.modules / 'gfx1201', game / 'lmxxf-modules')
        (game / 'LmxxfNrRuntime.dll').write_bytes(b'old runtime')
        before = snapshot_files(game)
        code, out = self.stage(game)
        self.assertEqual(code, 1, out)
        self.assertIn('Uninstall the old installation first', out)
        self.assertEqual(snapshot_files(game), before)

    def test_developer_staging_rejects_corrupt_source_before_changes(self):
        game = self.root / 'game'
        make_modules(game / 'lmxxf-modules', marker='old')
        (game / 'LmxxfNrRuntime.dll').write_bytes(b'old runtime')
        before = snapshot_files(game)
        damage_modules(self.modules, 'corrupt')
        code, out = self.stage(game)
        self.assertEqual(code, 1, out)
        self.assertIn('checksum mismatch', out)
        self.assertEqual(snapshot_files(game), before)

    def test_developer_staging_succeeds(self):
        game = self.root / 'new game'
        code, out = self.stage(game)
        self.assertEqual(code, 0, out)
        self.assertEqual(snapshot_files(game / 'lmxxf-modules'), snapshot_files(self.modules))
        self.assertTrue((game / 'LmxxfNrRuntime.dll').is_file())


if __name__ == '__main__':
    unittest.main(verbosity=2)
