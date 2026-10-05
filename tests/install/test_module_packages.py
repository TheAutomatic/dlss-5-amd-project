"""Run the real packaging/staging entrypoints with tiny, non-executable inputs."""
import os
import configparser
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
import runpy

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_lib'))
from lmxxf_fixtures import damage_modules, make_modules, snapshot_files


REPO = Path(__file__).resolve().parents[2]
PS = os.environ.get('LMXXF_TEST_POWERSHELL', shutil.which('powershell.exe'))


@unittest.skipUnless(os.name == 'nt' and PS, 'Windows PowerShell required')
class ModulePackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='lmxxf package entrypoints ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'tools/release').mkdir(parents=True)
        (self.root / 'tools/lmxxf-sync').mkdir(parents=True)
        for name in ('release/PACKAGE_RELEASE.ps1', 'install-amd-presr.ps1', 'uninstall-amd-presr.ps1',
                     'lmxxf-module-package.ps1', 'stage-lmxxf-beside-optiscaler.cmd',
                     'stage-lmxxf-beside-optiscaler.ps1', 'release/check-module-contract.ps1',
                     'release/check-release-freshness.ps1', 'lmxxf-sync/manifest.json'):
            shutil.copy2(REPO / 'tools' / name, self.root / 'tools' / name)
        # Both real gates run against this synthetic checkout. Carry their source inputs
        # along with the entrypoint instead of replacing a new gate with a success stub.
        for name in (
            'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrRuntime.cpp',
            'tests/lmxxf/lmxxf_nr_abi.cpp', 'tests/lmxxf/test_runtime_validation.py', 'tests/_lib/lmxxf_fixtures.py',
            'third_party/lmxxf/hip/build-modules.ps1',
            # module_headers listed in tools/lmxxf-sync/manifest.json; freshness fingerprints them.
            'third_party/lmxxf/Development/HIP/swin_persistent_types.h',
        ):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / name, target)
        if shutil.which('git'):
            subprocess.run(['git', 'init', '-q', str(self.root)], check=True, capture_output=True)
        for name in ('README.md', 'README.en.md', 'README.es.md'):
            shutil.copy2(REPO / name, self.root / name)
        (self.root / 'OptiScaler-DLSSNR-PreSR-Multipass-main').mkdir(exist_ok=True)
        shutil.copy2(REPO / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler.ini',
                     self.root / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler.ini')
        self.opti = self.root / 'fixture-opti.dll'
        self.opti.write_bytes(b'fixture proxy, not executable')
        runtime = self.root / 'exports/lmxxf-runtime/LmxxfNrRuntime.dll'
        runtime.parent.mkdir(parents=True)
        runtime.write_bytes(b'fixture runtime, not executable')
        (runtime.parent / 'runtime-ci.sha256').write_text(hashlib.sha256(runtime.read_bytes()).hexdigest())
        self.modules = make_modules(self.root / 'third_party/lmxxf/modules')
        shaders = self.root / 'third_party/lmxxf/shaders'
        shaders.mkdir()
        (shaders / 'native_codec_encode.hlsl').write_text('// fixture', encoding='utf-8')
        self.env = {k.upper(): v for k, v in os.environ.items()}
        self.env.pop('PSMODULEPATH', None)
        self.archive = self.root / 'dist/package.zip'
        # Real Mochizuki source/hash gate, with explicitly non-executable fixture artifacts.
        for directory in ('third_party/mochizuki',
                          'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/mochizuki_runtime'):
            shutil.copytree(REPO / directory, self.root / directory, ignore=shutil.ignore_patterns('__pycache__'))
        for name in ('OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/library/vulkan/vulkan-1.lib',
                     'tools/build/mochizuki-manifest.py', 'tools/build/build-mochizuki-runtime.py',
                     'tools/build/build-mochizuki-runtime.cmd', 'tools/build/mochizuki-deps.py',
                     'tools/install/mochizuki-model.py', 'tools/install/mochizuki-python.ps1', 'docs/mochizuki.md',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/NrPerformance.h'):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / name, target)
        mz = self.root / 'exports/mochizuki-runtime'
        (mz / 'dlssnr-amd/shaders').mkdir(parents=True)
        (mz / 'MochizukiNrRuntime.dll').write_bytes(b'fixture Vulkan runtime, not executable')
        (mz / 'dlssnr-amd/shaders/test.spv').write_bytes(b'fixture SPIR-V, not executable')
        runpy.run_path(str(self.root / 'tools/build/mochizuki-manifest.py'))['write'](mz)
        (mz / 'abi-ci.sha256').write_text(hashlib.sha256((mz / 'MochizukiNrRuntime.dll').read_bytes()).hexdigest())

    def test_mochizuki_runtime_changed_after_abi_is_rejected(self):
        runtime = self.root / 'exports/mochizuki-runtime/MochizukiNrRuntime.dll'
        runtime.write_bytes(b'changed fixture')
        code, out = self.package()
        self.assertNotEqual(code, 0, out)
        self.assertIn('Mochizuki source/artifact mismatch', out)
        self.assertFalse(self.archive.exists())

    def run_ps(self, args, env=None):
        result = subprocess.run([PS, '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', *args],
                                cwd=self.root, env=env or self.env, capture_output=True, timeout=60)
        return result.returncode, (result.stdout + result.stderr).decode('utf-8', errors='replace')

    def package(self):
        return self.run_ps(['-File', str(self.root / 'tools/release/PACKAGE_RELEASE.ps1'),
                           '-OptiDll', str(self.opti), '-AllowMissingDeps', '-Name', 'package'])

    def assert_rejected(self, fault):
        damage_modules(self.modules, fault)
        code, out = self.package()
        self.assertNotEqual(code, 0, out)
        self.assertFalse(self.archive.exists(), out)
        return out

    def test_runtime_changed_after_ci_is_rejected_before_staging(self):
        runtime = self.root / 'exports/lmxxf-runtime/LmxxfNrRuntime.dll'
        runtime.write_bytes(b'different fresh runtime')
        code, out = self.package()
        self.assertNotEqual(code, 0, out)
        self.assertIn('Runtime differs', out)
        self.assertFalse((self.root / 'dist/package').exists())

    def test_runtime_local_headers_invalidate_old_dll(self):
        runtime = self.root / 'exports/lmxxf-runtime/LmxxfNrRuntime.dll'
        source_dir = self.root / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime'
        for name in ('LmxxfExposureMeter.h', 'LmxxfRecordingLease.h', 'future/RuntimeHelper.h'):
            with self.subTest(header=name):
                header = source_dir / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text('// modified runtime dependency\n', encoding='utf-8')
                newer = runtime.stat().st_mtime + 10
                os.utime(header, (newer, newer))
                code, out = self.package()
                self.assertNotEqual(code, 0, out)
                self.assertIn('STALE LmxxfNrRuntime.dll', out)
                self.assertIn(header.name, out)
                header.unlink()

    def test_menu_assets_invalidate_old_host(self):
        source_dir = self.root / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler'
        for name in ('menu/MenuStrings.inl', 'menu/font/Subset.ttf', 'OptiScaler.rc', 'OptiScaler.vcxproj'):
            with self.subTest(source=name):
                source = source_dir / name
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_bytes(b'fixture build input')
                newer = self.opti.stat().st_mtime + 20
                os.utime(source, (newer, newer))
                code, out = self.package()
                self.assertNotEqual(code, 0, out)
                self.assertIn('STALE OptiScaler.dll', out)
                self.assertIn(source.name, out)
                source.unlink()

    def test_runtime_without_ci_proof_is_rejected(self):
        (self.root / 'exports/lmxxf-runtime/runtime-ci.sha256').unlink()
        code, out = self.package()
        self.assertNotEqual(code, 0, out)
        self.assertIn('Missing runtime-ci.sha256', out)
        self.assertFalse(self.archive.exists())

    def test_archive_preserves_nr_multiplier_default(self):
        code, out = self.package()
        self.assertEqual(code, 0, out)
        source = configparser.ConfigParser(strict=False)
        source.read(REPO / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler.ini', encoding='utf-8-sig')
        with zipfile.ZipFile(self.archive) as archive:
            packaged = configparser.ConfigParser(strict=False)
            packaged.read_string(archive.read('OptiScaler.ini').decode('utf-8-sig'))
        self.assertEqual(packaged['DlssNr']['XeFGInterpolationCount'], 'auto')
        self.assertEqual(packaged['DlssNr']['XeFGInterpolationCount'],
                         source['DlssNr']['XeFGInterpolationCount'])
        self.assertEqual(packaged['XeFG']['InterpolationCount'], source['XeFG']['InterpolationCount'])

    def test_valid_actual_archive_can_install(self):
        code, out = self.package()
        self.assertEqual(code, 0, out)
        extracted = self.root / 'extracted'
        with zipfile.ZipFile(self.archive) as archive:
            entries = archive.namelist()
            self.assertEqual(sum(p.endswith('.hsaco') for p in entries), 76)
            self.assertIn('lmxxf-module-package.ps1', entries)
            archive.extractall(extracted)
        game = self.root / 'game'
        game.mkdir()
        code, out = self.run_ps(['-File', str(extracted / 'Setup.ps1'), '-GameDir', str(game), '-NonInteractive'])
        self.assertEqual(code, 0, out)
        self.assertIn('MODEL SETUP REQUIRED', out)
        self.assertNotIn('Install SUCCEEDED', out)
        installed_ini = (game / 'OptiScaler.ini').read_text(encoding='utf-8-sig')
        # Check the installed artifact: the packager replaces the source DlssNr section.
        # A source-template-only addition used to disappear from the shipped ini.
        expected = {
            'NrTimingEnabled': 'false', 'NrTimingLog': 'false',
            'NrOverallIntensity': '1.0', 'NrStabilizerEnabled': 'false',
            'NrStabilizerAlpha': '0.8', 'NrStabilizerThreshold': '4.0',
            'MochizukiStyle': '0', 'MochizukiModelScale': '1.0',
            'MochizukiTemporal': 'true', 'MochizukiPreprocess': 'false',
            'MochizukiDynamicResolution': '1', 'MochizukiPass2Override': 'false',
            'DLSS5_STYLE': '1', 'DLSS5_NETWORK_1080_ROWS': '1152',
            'DLSS5_NETWORK_FREE_RES': 'true', 'DLSS5_FAST_NUMERIC': 'true',
            'DLSS5_MULTI_PASS': '1', 'DLSS5_MULTI_PASS_SKIP_BLOCKS': 'none',
            'DLSS5_SKIP_BLOCKS': 'none', 'DLSS5_FORMAT_FALLBACK': 'true', 'DLSS5_NETWORK_HEIGHT': 'auto',
            'DLSS5_HIP_SHARED_POOL': 'true', 'DLSS5_HIP_MH_BYTE_STREAM': 'true',
            'DLSS5_HIP_DECODER_BYTE': 'true',
        }
        for path in (game / 'OptiScaler.ini', extracted / 'OptiScaler.ini',
                     REPO / 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler.ini'):
            parsed = configparser.ConfigParser(interpolation=None, strict=True)
            parsed.read_string(path.read_text(encoding='utf-8-sig'))
            for key, value in expected.items():
                self.assertEqual(parsed['DlssNr'][key], value, f'{path}: {key}')
        for key, value in {'DLSS5_VIT_ADAPTIVE': '1', 'DLSS5_VIT_REUSE_PERIOD': '16',
                           'DLSS5_VIT_REUSE_GLOBAL': '1', 'DLSS5_VIT_REUSE_LOCAL': '50',
                           'DLSS5_VIT_REUSE_IMAGE': '1'}.items():
            self.assertIn(key + '=' + value, installed_ini)

        self.assertEqual(snapshot_files(game / 'lmxxf-modules'), snapshot_files(self.modules))

        code, out = self.run_ps(['-File', str(extracted / 'Setup.ps1'), '-GameDir', str(game),
                                 '-NonInteractive', '-UninstallExisting'])
        self.assertEqual(code, 0, out)
        self.assertIn('Uninstall SUCCEEDED', out)
        self.assertIn('MODEL SETUP REQUIRED', out)
        self.assertNotIn('Install SUCCEEDED', out)
        self.assertEqual(snapshot_files(game / 'lmxxf-modules'), snapshot_files(self.modules))

        # A complete generated package must also uninstall its code/shader trees,
        # including a nested shader left by an earlier version.
        stale = game / 'lmxxf-modules/shaders/native_game_rgb_input.hlsl'
        stale.parent.mkdir()
        stale.write_text('// old installed shader', encoding='utf-8')
        code, out = self.run_ps(['-File', str(extracted / 'Uninstall_OptiScaler_NR.ps1'),
                                 '-GameDir', str(game), '-NonInteractive', '-NoPause'])
        self.assertEqual(code, 0, out)
        self.assertIn('Uninstall SUCCEEDED', out)
        for relative in ('LmxxfNrRuntime.dll', 'MochizukiNrRuntime.dll', 'lmxxf-modules', 'shaders'):
            self.assertFalse((game / relative).exists(), relative)

    def test_mochizuki_only_install_preserves_model_on_uninstall(self):
        code, out = self.package()
        self.assertEqual(code, 0, out)
        extracted = self.root / 'extracted'
        with zipfile.ZipFile(self.archive) as archive:
            self.assertIn('MochizukiNrRuntime.dll', archive.namelist())
            self.assertEqual(archive.read('mochizuki-python.ps1'),
                             (REPO / 'tools/install/mochizuki-python.ps1').read_bytes())
            launcher = archive.read('Mochizuki-Model.bat').decode('ascii')
            self.assertIn('-File "%~dp0mochizuki-python.ps1"', launcher)
            self.assertIn('exit /b %RC%', launcher)
            self.assertNotIn('dlssnr-amd/dlssnr.bin', archive.namelist())
            archive.extractall(extracted)
        model = extracted / 'dlssnr-amd/dlssnr.bin'
        # Recognized header; this lifecycle fixture is not an inference model.
        model.write_bytes(b'NRMODEL1' + (599).to_bytes(4, 'little') + b'user-owned model fixture')
        game = self.root / 'mochizuki game'
        game.mkdir()
        code, out = self.run_ps(['-File', str(extracted / 'Setup.ps1'), '-GameDir', str(game),
                                '-Backend', 'mochizuki', '-NonInteractive'])
        self.assertEqual(code, 0, out)
        self.assertTrue((game / 'MochizukiNrRuntime.dll').is_file())
        self.assertFalse((game / 'LmxxfNrRuntime.dll').exists())
        self.assertFalse((game / 'dlssnr_amd_pass1.dll').exists())
        parsed = configparser.ConfigParser()
        parsed.read(game / 'OptiScaler.ini', encoding='utf-8-sig')
        self.assertEqual(parsed['DlssNr']['NrBackend'], 'mochizuki')
        self.assertEqual((game / 'dlssnr-amd/dlssnr.bin').read_bytes(), model.read_bytes())
        code, out = self.run_ps(['-File', str(extracted / 'Uninstall_OptiScaler_NR.ps1'), '-GameDir', str(game), '-NonInteractive'])
        self.assertEqual(code, 0, out)
        self.assertFalse((game / 'MochizukiNrRuntime.dll').exists())
        self.assertEqual((game / 'dlssnr-amd/dlssnr.bin').read_bytes(), model.read_bytes())

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
        env['LMXXF_PACKAGE_ENTRY'] = str(self.root / 'tools/release/PACKAGE_RELEASE.ps1')
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
