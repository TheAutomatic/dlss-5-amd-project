"""Real packaging/staging gates with synthetic modules and a tiny signable PE."""
import os
import configparser
import hashlib
import json
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
from mochizuki_fixtures import int4_tools


REPO = Path(__file__).resolve().parents[2]
PS = os.environ.get('LMXXF_TEST_POWERSHELL', shutil.which('powershell.exe'))


@unittest.skipUnless(os.name == 'nt' and PS, 'Windows PowerShell required')
class ModulePackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = REPO / 'work/scratch'
        scratch.mkdir(parents=True, exist_ok=True)
        cls.signing_temp = tempfile.TemporaryDirectory(prefix='package-signing-fixture-', dir=scratch)
        cls.addClassCleanup(cls.signing_temp.cleanup)
        env = dict(os.environ, PACKAGE_FIXTURE_DIR=cls.signing_temp.name,
                   PACKAGE_SIGNING_HELPER=str(REPO / 'tools/release/package-signing.ps1'))
        script = '''$ErrorActionPreference = 'Stop'
. $env:PACKAGE_SIGNING_HELPER
Add-Type -TypeDefinition 'public static class PackageFixture { public static int Value() { return 42; } }' -OutputAssembly (Join-Path $env:PACKAGE_FIXTURE_DIR 'fixture.dll')
(Get-PackageSigningCertificate -Ephemeral).Thumbprint
'''
        result = subprocess.run([PS, '-NoProfile', '-Command', script], env=env,
                                capture_output=True, check=True, timeout=60)
        cls.thumbprint = result.stdout.decode().strip()
        cls.addClassCleanup(cls.remove_certificate)
        cls.pe_bytes = (Path(cls.signing_temp.name) / 'fixture.dll').read_bytes()

    @classmethod
    def remove_certificate(cls):
        subprocess.run([PS, '-NoProfile', '-Command',
                        'Import-Module (Join-Path $PSHOME "Modules/Microsoft.PowerShell.Security/Microsoft.PowerShell.Security.psd1"); '
                        'Get-Item -LiteralPath ("Cert:/CurrentUser/My/" + $env:PACKAGE_TEST_CERT) -ErrorAction Stop | Out-Null; '
                        'Remove-Item -LiteralPath ("Cert:/CurrentUser/My/" + $env:PACKAGE_TEST_CERT) -DeleteKey -ErrorAction Stop'],
                       env=dict(os.environ, PACKAGE_TEST_CERT=cls.thumbprint), check=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='lmxxf package entrypoints ', dir=REPO / 'work/scratch')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'tools/release').mkdir(parents=True)
        (self.root / 'tools/lmxxf-sync').mkdir(parents=True)
        (self.root / 'tools/install').mkdir(parents=True, exist_ok=True)
        (self.root / 'tools/build').mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / 'tools/install/mochizuki-owned.txt', self.root / 'tools/install/mochizuki-owned.txt')
        for name in ('release/PACKAGE_RELEASE.ps1', 'install-amd-presr.ps1', 'uninstall-amd-presr.ps1',
                     'lmxxf-module-package.ps1', 'stage-lmxxf-beside-optiscaler.cmd',
                     'stage-lmxxf-beside-optiscaler.ps1', 'release/check-module-contract.ps1',
                     'release/check-release-freshness.ps1', 'release/package-signing.ps1', 'lmxxf-sync/manifest.json',
                     'build/build-launcher.ps1', 'install/launcher-worker.ps1', 'install/proxy-probe.ps1',
                     'install/library-scan.ps1', 'install/install-capabilities.ps1',
                     'install/lmxxf-model.py', 'install/lmxxf-small.ps1', 'install/lmxxf-small-manifest.json'):
            shutil.copy2(REPO / 'tools' / name, self.root / 'tools' / name)
        shutil.copytree(REPO / 'tools/install/launcher', self.root / 'tools/install/launcher')
        shutil.copytree(REPO / 'assets/compatibility', self.root / 'assets/compatibility')
        shutil.copy2(REPO / 'tools/install/xess-compat.ps1', self.root / 'tools/install/xess-compat.ps1')
        for name in ('LICENSE', 'NOTICE', 'OptiScaler-DLSSNR-PreSR-Multipass-main/LICENSE',
                     'third_party/lmxxf/LICENSE', 'third_party/onnxruntime/LICENSE',
                     'third_party/onnxruntime/YOLO11_AGPL-3.0.txt',
                     'third_party/onnxruntime/PPHumanSeg_Apache-2.0.txt', 'third_party/onnxruntime/YuNet_MIT.txt',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/external/xess/LICENSE.txt',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/external/FidelityFX-SDK/docs/license.md',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/external/FidelityFX-SDK-v2/docs/license.md',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/external/directx_agility_sdk/LICENSE.txt'):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / name, target)
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
        self.opti.write_bytes(self.pe_bytes)
        runtime = self.root / 'exports/lmxxf-runtime/LmxxfNrRuntime.dll'
        runtime.parent.mkdir(parents=True)
        runtime.write_bytes(self.pe_bytes)
        (runtime.parent / 'runtime-ci.sha256').write_text(hashlib.sha256(runtime.read_bytes()).hexdigest())
        self.modules = make_modules(self.root / 'third_party/lmxxf/modules')
        shaders = self.root / 'third_party/lmxxf/shaders'
        shaders.mkdir()
        (shaders / 'native_codec_encode.hlsl').write_text('// fixture', encoding='utf-8')
        self.env = {k.upper(): v for k, v in os.environ.items()}
        self.env.pop('PSMODULEPATH', None)
        self.archive = self.root / 'dist/package.zip'
        # Official ASI payload is vendored; packaging checks names/layout only.
        payload = self.root / 'third_party/mochizuki-asi/payload'
        (payload / 'OptiScaler/plugins').mkdir(parents=True)
        (payload / 'OptiScaler/plugins/dlssnr-amd.asi').write_bytes(self.pe_bytes)
        (payload / 'nr_present_probe.exe').write_bytes(self.pe_bytes)
        (payload / 'dlssnr-amd/shaders').mkdir(parents=True)
        (payload / 'dlssnr-amd/shaders/g_attn.spv').write_bytes(b'fixture network SPIR-V')
        (payload / 'dlssnr-amd/shaders/accumulation.txt').write_text('fixture accumulation\n', encoding='utf-8')
        aco = payload / 'dlssnr-amd/aco'
        (aco / 'records').mkdir(parents=True)
        (aco / 'shaders').mkdir(parents=True)
        (aco / 'records/0123456789abcdef.nrp').write_bytes(b'NRPAL fixture record')
        (aco / 'shell-aliases.txt').write_text('g_from.spv g_attn.spv\n', encoding='utf-8')
        (aco / 'shaders/shader-constants.txt').write_bytes(b'fixture constants\n')
        (payload / 'dlssnr-amd/int4').mkdir(parents=True)
        (payload / 'dlssnr-amd/int4/settings.txt').write_text('fixture\n', encoding='utf-8')
        (payload / 'model-tools').mkdir(parents=True)
        (payload / 'model-tools/dlssnr_extract_model.exe').write_bytes(self.pe_bytes)
        (payload / 'model-tools/dlssnr-int4-weights.exe').write_bytes(self.pe_bytes)
        (payload / 'ini').mkdir(parents=True)
        (payload / 'ini/dlssnr-amd-optiscaler.ini').write_text('[Network]\nModel = CC_SMALL_FP8\n', encoding='utf-8')
        (payload / 'Licenses').mkdir(parents=True)
        (payload / 'Licenses/DearImGui-LICENSE.txt').write_text('fixture license\n', encoding='utf-8')
        self.asi_payload = payload
        for name in ('VERSION', 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/library/vulkan/vulkan-1.lib',
                     'docs/mochizuki.md', 'docs/lmxxf-small-model.md', 'docs/lmxxf-044-consumer-review.md',
                     'third_party/mochizuki/LICENSE', 'third_party/mochizuki/UPSTREAM.md',
                     'third_party/mochizuki/windows/src/core/renodx/LICENSE',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/NrPerformance.h'):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / name, target)
        # Packager copies the ASI sources notes and legacy ownership pin.
        (self.root / 'third_party/mochizuki-asi').mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / 'third_party/mochizuki-asi/README.md',
                     self.root / 'third_party/mochizuki-asi/README.md')

    def test_package_binds_asi_plugin_and_resources(self):
        code, out = self.package()
        self.assertEqual(code, 0, out)
        payload = self.asi_payload
        with zipfile.ZipFile(self.archive) as archive:
            by_name = {entry.replace('\\', '/'): entry for entry in archive.namelist()}
            def read(relative):
                return archive.read(by_name[relative])
            self.assertIn('OptiScaler/plugins/dlssnr-amd.asi', by_name)
            signatures = json.loads(read('SIGNATURES.json'))
            signed = {item['file']: item for item in signatures['binaries']}
            asi = signed['OptiScaler/plugins/dlssnr-amd.asi']
            self.assertEqual(asi['inputSha256'].lower(),
                             hashlib.sha256((payload / 'OptiScaler/plugins/dlssnr-amd.asi').read_bytes()).hexdigest())
            self.assertEqual(asi['signedSha256'].lower(), hashlib.sha256(read('OptiScaler/plugins/dlssnr-amd.asi')).hexdigest())
            self.assertEqual(read('dlssnr-amd/shaders/g_attn.spv'),
                             (payload / 'dlssnr-amd/shaders/g_attn.spv').read_bytes())
            self.assertEqual(read('dlssnr-amd/aco/shell-aliases.txt'),
                             (payload / 'dlssnr-amd/aco/shell-aliases.txt').read_bytes())
            self.assertIn('dlssnr-amd/nr_present_probe.exe', by_name)
            self.assertIn('model-tools/dlssnr_extract_model.exe', by_name)
            self.assertIn('model-tools/dlssnr-int4-weights.exe', by_name)
            self.assertIn('dlssnr-amd-optiscaler.ini', by_name)
            self.assertIn('Mochizuki-Model.bat', by_name)
            self.assertNotIn('MochizukiNrRuntime.dll', by_name)
            self.assertNotIn('dlssnr-amd/dlssnr.bin', by_name)
            launcher = read('Mochizuki-Model.bat').decode('ascii')
            self.assertIn('dlssnr_extract_model.exe', launcher)
            self.assertNotIn('mochizuki-python.ps1', launcher)

    def test_missing_license_rejected_before_staging(self):
        for name in ('third_party/lmxxf/LICENSE', 'third_party/onnxruntime/LICENSE',
                     'third_party/onnxruntime/YOLO11_AGPL-3.0.txt',
                     'third_party/onnxruntime/PPHumanSeg_Apache-2.0.txt'):
            with self.subTest(name=name):
                path = self.root / name
                contents = path.read_bytes()
                path.unlink()
                code, out = self.package()
                self.assertNotEqual(code, 0, out)
                self.assertIn('Required license missing: ' + name, out)
                self.assertFalse(self.archive.exists())
                path.write_bytes(contents)

    def test_package_carries_required_notices(self):
        code, out = self.package()
        self.assertEqual(code, 0, out)
        with zipfile.ZipFile(self.archive) as archive:
            for source, dest in [('LICENSE', 'LICENSE'), ('NOTICE', 'NOTICE'),
                                 ('third_party/lmxxf/LICENSE', 'Licenses/Lmxxf_MIT.txt'),
                                 ('third_party/mochizuki/LICENSE', 'Licenses/Mochizuki_MIT.txt')]:
                self.assertEqual(archive.read(dest), (self.root / source).read_bytes())

    def run_ps(self, args, env=None, timeout=60):
        try:
            result = subprocess.run([PS, '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', *args],
                                    cwd=self.root, env=env or self.env, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            output = ((error.stdout or b'') + (error.stderr or b'')).decode('utf-8', errors='replace')
            self.fail(f'PowerShell timed out after {timeout}s: {args}\nLast output:\n{output[-12000:]}')
        return result.returncode, (result.stdout + result.stderr).decode('utf-8', errors='replace')

    def package(self):
        # Real signing and ZIP verification share the runner with host LTO.
        # The old 60-second process limit killed a healthy release smoke run.
        return self.run_ps(['-File', str(self.root / 'tools/release/PACKAGE_RELEASE.ps1'),
                           '-OptiDll', str(self.opti), '-AllowMissingDeps', '-Name', 'package',
                           '-SigningThumbprint', self.thumbprint], timeout=180)

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

    def test_archive_preserves_language_and_history_defaults(self):
        code, out = self.package()
        self.assertEqual(code, 0, out)
        with zipfile.ZipFile(self.archive) as archive:
            ini = archive.read('OptiScaler.ini').decode('utf-8-sig')
            cfg = configparser.ConfigParser(strict=False)
            cfg.read_string(ini)
            self.assertEqual(cfg['Menu']['Language'], 'en')
            self.assertEqual(cfg['DlssNr']['LmxxfModelHistory'], 'true')
            self.assertTrue(cfg.getboolean('DlssNr', 'DLSS5_VIT_ADAPTIVE'))
            self.assertIn('Temporarily disables adaptive ViT reuse', ini)
            self.assertIn('the saved reuse preference is retained', ini)

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
            self.assertEqual(sum(p.endswith('.hsaco') for p in entries), 80)
            self.assertIn('lmxxf-module-package.ps1', entries)
            archive.extractall(extracted)
        game = self.root / 'game'
        game.mkdir()
        code, out = self.run_ps(['-File', str(extracted / 'Setup.ps1'), '-GameDir', str(game), '-NonInteractive', '-Backend', 'lmxxf'])
        self.assertEqual(code, 0, out)
        self.assertIn('MODEL SETUP REQUIRED', out)
        self.assertNotIn('Install SUCCEEDED', out)
        installed_ini = (game / 'OptiScaler.ini').read_text(encoding='utf-8-sig')
        # Check the installed artifact: the packager replaces the source DlssNr section.
        # A source-template-only addition used to disappear from the shipped ini.
        expected = {
            'NrTimingEnabled': 'false', 'NrTimingLog': 'false',
            'NrOverallIntensity': '1.0', 'NrStabilizerEnabled': 'false',
            'NrResidualLowGain': '1.0', 'NrResidualDetailGain': '1.0',
            'NrResidualSkinProtection': '0.0', 'NrResidualEdgeProtection': '0.0',
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
                                 '-NonInteractive', '-UninstallExisting', '-Backend', 'lmxxf'])
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

    def test_mochizuki_asi_install_and_uninstall_inventory(self):
        code, out = self.package()
        self.assertEqual(code, 0, out)
        extracted = self.root / 'extracted'
        with zipfile.ZipFile(self.archive) as archive:
            self.assertIn('OptiScaler/plugins/dlssnr-amd.asi', archive.namelist())
            self.assertIn('dlssnr-amd/shaders/g_attn.spv', archive.namelist())
            self.assertNotIn('MochizukiNrRuntime.dll', archive.namelist())
            self.assertNotIn('dlssnr-amd/dlssnr.bin', archive.namelist())
            archive.extractall(extracted)
        model = extracted / 'dlssnr-amd/dlssnr.bin'
        model.write_bytes(b'user supplied official model')
        game = self.root / 'mochizuki game'
        game.mkdir()
        code, out = self.run_ps(['-File', str(extracted / 'Setup.ps1'), '-GameDir', str(game),
                                '-Backend', 'mochizuki', '-MochizukiModels', 'original', '-NonInteractive'])
        self.assertEqual(code, 0, out)
        self.assertFalse((game / 'MochizukiNrRuntime.dll').exists())
        asi = list(game.rglob('dlssnr-amd.asi'))
        self.assertEqual(len(asi), 1, out)
        self.assertEqual((game / 'dlssnr-amd/shaders/g_attn.spv').read_bytes(),
                         (extracted / 'dlssnr-amd/shaders/g_attn.spv').read_bytes())
        self.assertEqual((game / 'dlssnr-amd/dlssnr.bin').read_bytes(), model.read_bytes())
        parsed = configparser.ConfigParser()
        parsed.read(game / 'OptiScaler.ini', encoding='utf-8-sig')
        self.assertEqual(parsed['DlssNr']['NrBackend'], 'mochizuki')
        self.assertEqual(parsed['Plugins']['LoadAsiPlugins'].lower(), 'true')
        user_asset = game / 'dlssnr-amd/shaders/user.spv'
        user_asset.write_bytes(b'user shader')
        lookalike = game / 'dlssnr-amd/aco/records/ffffffffffffffff.nrp'
        lookalike.write_bytes(b'user record')
        code, out = self.run_ps(['-File', str(extracted / 'Uninstall_OptiScaler_NR.ps1'), '-GameDir', str(game), '-NonInteractive'])
        self.assertEqual(code, 0, out)
        self.assertFalse(asi[0].exists())
        self.assertFalse((game / 'dlssnr-amd/shaders/g_attn.spv').exists())
        self.assertEqual((game / 'dlssnr-amd/dlssnr.bin').read_bytes(), model.read_bytes())
        self.assertEqual(user_asset.read_bytes(), b'user shader')
        self.assertEqual(lookalike.read_bytes(), b'user record')

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
