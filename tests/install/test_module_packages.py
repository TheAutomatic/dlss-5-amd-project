"""Real packaging/staging gates with a tiny signable PE."""
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

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_lib'))


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
        self.temp = tempfile.TemporaryDirectory(prefix='package entrypoints ', dir=REPO / 'work/scratch')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'tools/release').mkdir(parents=True)
        (self.root / 'tools/install').mkdir(parents=True, exist_ok=True)
        (self.root / 'tools/build').mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / 'tools/install/mochizuki-owned.txt', self.root / 'tools/install/mochizuki-owned.txt')
        for name in ('release/PACKAGE_RELEASE.ps1', 'install-amd-presr.ps1', 'uninstall-amd-presr.ps1',
                     'release/check-release-freshness.ps1', 'release/package-signing.ps1',
                     'build/build-launcher.ps1', 'install/launcher-worker.ps1', 'install/proxy-probe.ps1',
                     'install/library-scan.ps1', 'install/install-capabilities.ps1'):
            shutil.copy2(REPO / 'tools' / name, self.root / 'tools' / name)
        shutil.copytree(REPO / 'tools/install/launcher', self.root / 'tools/install/launcher')
        shutil.copytree(REPO / 'assets/compatibility', self.root / 'assets/compatibility')
        shutil.copy2(REPO / 'tools/install/xess-compat.ps1', self.root / 'tools/install/xess-compat.ps1')
        for name in ('LICENSE', 'NOTICE', 'OptiScaler-DLSSNR-PreSR-Multipass-main/LICENSE',
                     'third_party/openrr/LICENSE', 'third_party/fsr-rr-1.2.1/license.md', 'third_party/fsr-rr-1.2.1/3rdpartynotice.md',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/external/xess/LICENSE.txt',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/external/FidelityFX-SDK/docs/license.md',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/external/FidelityFX-SDK-v2/docs/license.md',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/external/directx_agility_sdk/LICENSE.txt'):
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
        openrr_dll = self.root / 'exports/openrr-runtime/OpenNrRrRuntime.dll'
        openrr_shaders = self.root / 'exports/openrr-runtime/opennr-rr/shaders'
        openrr_shaders.mkdir(parents=True)
        openrr_dll.write_bytes(self.pe_bytes)
        (openrr_shaders / 'ingest.spv').write_bytes(b'fixture openrr spv')
        self.env = {k.upper(): v for k, v in os.environ.items()}
        self.env.pop('PSMODULEPATH', None)
        self.archive = self.root / 'dist/package.zip'
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
        (payload / 'experimental').mkdir(parents=True)
        (payload / 'experimental/dlssnr-small-experimental.delta').write_bytes(b'NRXDIFF1' + (0).to_bytes(4, 'little'))
        (payload / 'ini').mkdir(parents=True)
        (payload / 'ini/dlssnr-amd-optiscaler.ini').write_text('[Network]\nModel = CC_SMALL_FP8\n', encoding='utf-8')
        (payload / 'ini/dlssnr-amd-optiscaler-renodx.ini').write_text('[Network]\nModel = CC_SMALL_FP8\n[RenoDX]\nEnabled = 1\n', encoding='utf-8')
        (payload / 'Licenses').mkdir(parents=True)
        (payload / 'Licenses/DearImGui-LICENSE.txt').write_text('fixture license\n', encoding='utf-8')
        self.asi_payload = payload
        for name in ('VERSION', 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/library/vulkan/vulkan-1.lib',
                     'docs/mochizuki.md',
                     'third_party/mochizuki-asi/LICENSE', 'third_party/mochizuki-asi/UPSTREAM.md',
                     'third_party/mochizuki-asi/RenoDX_MIT.txt',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h',
                     'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/NrPerformance.h'):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / name, target)
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
            self.assertIn('experimental/dlssnr-small-experimental.delta', by_name)
            self.assertIn('dlssnr-amd-optiscaler-renodx.ini', by_name)
            self.assertNotIn('MochizukiNrRuntime.dll', by_name)
            self.assertNotIn('LmxxfNrRuntime.dll', by_name)
            self.assertNotIn('dlssnr-amd/dlssnr.bin', by_name)
            launcher = read('Mochizuki-Model.bat').decode('ascii')
            self.assertIn('dlssnr_extract_model.exe', launcher)
            self.assertNotIn('mochizuki-python.ps1', launcher)

    def test_missing_license_rejected_before_staging(self):
        for name in ('third_party/openrr/LICENSE', 'third_party/mochizuki-asi/LICENSE'):
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
                                 ('third_party/openrr/LICENSE', 'Licenses/OpenRR_MIT.txt'),
                                 ('third_party/mochizuki-asi/LICENSE', 'Licenses/Mochizuki_MIT.txt')]:
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
        return self.run_ps(['-File', str(self.root / 'tools/release/PACKAGE_RELEASE.ps1'),
                           '-OptiDll', str(self.opti), '-AllowMissingDeps', '-Name', 'package',
                           '-SigningThumbprint', self.thumbprint], timeout=180)

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

    def test_archive_preserves_language_and_history_defaults(self):
        code, out = self.package()
        self.assertEqual(code, 0, out)
        with zipfile.ZipFile(self.archive) as archive:
            ini = archive.read('OptiScaler.ini').decode('utf-8-sig')
            cfg = configparser.ConfigParser(strict=False)
            cfg.read_string(ini)
            self.assertEqual(cfg['Menu']['Language'], 'en')

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

    def test_mochizuki_asi_install_and_uninstall_inventory(self):
        code, out = self.package()
        self.assertEqual(code, 0, out)
        extracted = self.root / 'extracted'
        with zipfile.ZipFile(self.archive) as archive:
            self.assertIn('OptiScaler/plugins/dlssnr-amd.asi', archive.namelist())
            self.assertIn('dlssnr-amd/shaders/g_attn.spv', archive.namelist())
            self.assertNotIn('MochizukiNrRuntime.dll', archive.namelist())
            self.assertNotIn('LmxxfNrRuntime.dll', archive.namelist())
            self.assertNotIn('dlssnr-amd/CC_SILVER_AARDWOLD.bin', archive.namelist())
            archive.extractall(extracted)
        model = extracted / 'dlssnr-amd/CC_SILVER_AARDWOLD.bin'
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
        self.assertEqual((game / 'dlssnr-amd/CC_SILVER_AARDWOLD.bin').read_bytes(), model.read_bytes())
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
        self.assertEqual((game / 'dlssnr-amd/CC_SILVER_AARDWOLD.bin').read_bytes(), model.read_bytes())
        self.assertEqual(user_asset.read_bytes(), b'user shader')
        self.assertEqual(lookalike.read_bytes(), b'user record')


if __name__ == '__main__':
    unittest.main(verbosity=2)