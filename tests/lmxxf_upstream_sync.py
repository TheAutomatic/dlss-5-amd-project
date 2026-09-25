"""Offline regression tests. Temporary Git repos only: no fetch or GPU builds."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PS = os.environ.get('LMXXF_TEST_POWERSHELL', shutil.which('powershell.exe') or shutil.which('pwsh'))
spec = importlib.util.spec_from_file_location('lmxxf_audit', ROOT / 'tools/audit-lmxxf-enablements.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding='utf-8')


def git(directory, *args):
    return subprocess.check_output(['git', '-c', 'safe.directory=' + directory.as_posix(),
        '-C', str(directory), *args], stderr=subprocess.PIPE).decode('utf-8').strip()


def commit(directory):
    git(directory, 'add', '-A')
    git(directory, '-c', 'user.name=Sync Test', '-c', 'user.email=sync-test@example.invalid',
        'commit', '-qm', 'fixture')
    return git(directory, 'rev-parse', 'HEAD')


class Fixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='lmxxf sync tests ')
        self.folder = Path(self.temp.name).resolve()
        self.local = self.folder / 'local'
        self.up = self.folder / 'upstream'
        self.vendor = self.local / 'third_party/lmxxf'
        self.config = self.local / audit.CONFIG
        shutil.copytree(ROOT / audit.CONFIG, self.config)
        shutil.copy2(ROOT / 'tools/sync-lmxxf-upstream.ps1', self.local / 'tools')
        shutil.copy2(ROOT / 'tools/audit-lmxxf-enablements.py', self.local / 'tools')
        manifest = audit.read_json(self.config / 'manifest.json')
        for name in manifest['headers']:
            content = (ROOT / audit.VENDOR / name).read_text(encoding='utf-8')
            write(self.vendor / name, content)
            write(self.up / name, content)
        # Construct raw upstream from the independently maintained patches.
        for entry in manifest['pinned']:
            git(self.up, 'apply', '--reverse', str(self.config / 'patches' / entry['patch']))
        product = (ROOT / audit.OPTIONS).read_text(encoding='utf-8')
        write(self.local / audit.OPTIONS, product)
        write(self.up / 'src/LmxxfProductionOptions.h', product)
        for profile in audit.PROFILES:
            write(self.up / profile, 'DLSS5_HIP_GRAPH=0\nDLSS5_TEST_UNKNOWN=1\nDLSS5_NETWORK_HEIGHT=900\n')
        recipe = '$modules = @(\n' + '\n'.join(
            "@{ name = '%s'; defines = @(); sources = @('active.hip') }" % module
            for module in ('multihead-fast-padded-wave', 'multihead-fast-padded-wave-packed')) + '\n)\n'
        kernel = '#ifndef HIP_FFN_LINE_STORES\n#define HIP_FFN_LINE_STORES 0\n#endif\n#if defined(HIP_EXPERIMENT) && HIP_OTHER\n#endif\n'
        module = b'fixture hsaco bytes'
        module_hash = hashlib.sha256(module).hexdigest()
        for owner in (self.up, self.vendor):
            write(owner / 'hip/active.hip', kernel)
            write(owner / 'hip/build-modules.ps1', recipe)
            write(owner / 'hip/rtc_compile.cpp', '// fixture compiler\n')
            write(owner / 'hip/README.md', 'fixture\n')
            write(owner / 'hip/SHA256SUMS', module_hash + '  gfx1201/test.hsaco\n')
            write(owner / 'shaders/active.hlsl', '// fixture shader\n')
        (self.vendor / 'modules').mkdir()
        (self.vendor / 'modules/test.hsaco').write_bytes(module)
        write(self.vendor / 'modules/SHA256SUMS', module_hash + '  test.hsaco\n')
        write(self.up / 'Development/deployments/prod.ps1', "$defines = @('HIP_FFN_LINE_STORES 1')\n")
        write(self.up / 'Development/HIP/experiments/test.ps1', 'HIP_EXPERIMENT=1\n')
        git(self.up, 'init', '-q')
        git(self.up, 'config', 'core.autocrlf', 'false')
        self.base = commit(self.up)
        write(self.vendor / 'UPSTREAM.md', '# fixture — UTF-8 审阅\n\n- Commit: `' + self.base + '` (synced fixture)\n')
        self.pin_before = (self.vendor / 'UPSTREAM.md').read_bytes()
        write(self.up / 'shaders/active.hlsl', '// new upstream shader\n')
        self.target = commit(self.up)
        self.pinned = {entry['path']: (self.vendor / entry['path']).read_bytes() for entry in manifest['pinned']}
        # Only the subprocess is stubbed in orchestrator tests. AuditTests exercise real Git
        # collection and decision validation; synthetic approvals never leave the fixture.
        self.audit_stub = ('import json, os, pathlib, sys\n'
            "pathlib.Path(__file__).with_name('audit-invocation.json').write_text(json.dumps(sys.argv[1:]))\n"
            "sys.exit(int(os.environ.get('LMXXF_FIXTURE_AUDIT_EXIT', '0')))\n")
        write(self.local / 'tools/build-lmxxf-runtime.cmd', '@exit /b 0\n')

    def tearDown(self):
        # Verify the exact cleanup target before TemporaryDirectory recursively removes it.
        self.assertEqual(self.folder.parent, Path(tempfile.gettempdir()).resolve())
        self.assertTrue(self.folder.name.startswith('lmxxf sync tests '))
        self.assertFalse(self.folder.is_symlink())
        self.temp.cleanup()

    def sync(self, extra=(), audit_exit=0, allow_stale=True, skip_build=True, python=None, write_audit=True):
        if write_audit:
            write(self.local / 'tools/audit-lmxxf-enablements.py', self.audit_stub)
        args = [PS, '-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
            str(self.local / 'tools/sync-lmxxf-upstream.ps1'), '-UpstreamPath', str(self.up),
            '-UpstreamRef', 'HEAD', '-SkipUpstreamFetch', '-SkipModules', '-PythonPath', python or sys.executable]
        if skip_build:
            args.append('-SkipBuild')
        if allow_stale:
            args.append('-AllowStaleModules')
        args.extend(extra)
        env = os.environ.copy()
        env['LMXXF_FIXTURE_AUDIT_EXIT'] = str(audit_exit)
        return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding='utf-8', errors='replace', env=env, timeout=60)

    def assert_failed(self, result):
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertNotIn('Source sync and integration review complete:', result.stdout)
        self.assertEqual((self.vendor / 'UPSTREAM.md').read_bytes(), self.pin_before)


@unittest.skipUnless(PS and os.name == 'nt', 'PowerShell/Windows required')
class SyncTests(Fixture):
    def test_mirrors_retired_files_and_preserves_pinned_headers(self):
        retired = ('src/retired.h', 'Development/HIP/retired.h', 'hip/retired.hip', 'shaders/retired.hlsl')
        untouched = ('src/notes.txt', 'hip/notes.txt', 'shaders/shader-cache/cache.dxbc', 'shaders/retired-network/keep.hlsl')
        for rel in retired + untouched:
            write(self.vendor / rel, 'fixture\n')
        result = self.sync()
        self.assertEqual(result.returncode, 0, result.stdout)
        for rel in retired:
            self.assertFalse((self.vendor / rel).exists(), rel)
        for rel in untouched:
            self.assertTrue((self.vendor / rel).exists(), rel)
        for rel, content in self.pinned.items():
            self.assertEqual((self.vendor / rel).read_bytes(), content, rel)
        args = json.loads((self.local / 'tools/audit-invocation.json').read_text())
        self.assertEqual(args[1], self.target)
        self.assertIn('审阅', (self.vendor / 'UPSTREAM.md').read_text(encoding='utf-8'))

    def test_missing_required_header_fails_before_copy(self):
        (self.up / 'src/native_game_codec.h').unlink()
        commit(self.up)
        before = (self.vendor / 'shaders/active.hlsl').read_bytes()
        result = self.sync()
        self.assert_failed(result)
        self.assertIn('Required upstream paths disappeared', result.stdout)
        self.assertEqual((self.vendor / 'shaders/active.hlsl').read_bytes(), before)

    def test_missing_pinned_upstream_needs_update_switch_to_fail(self):
        (self.up / 'src/native_rgb_reflect.h').unlink()
        commit(self.up)
        self.assert_failed(self.sync(('-UpdateReflect',)))
        result = self.sync()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual((self.vendor / 'src/native_rgb_reflect.h').read_bytes(), self.pinned['src/native_rgb_reflect.h'])

    def test_update_switches_reproduce_local_headers(self):
        result = self.sync(('-UpdateBridge', '-UpdateReflect', '-UpdateInputGeometry'))
        self.assertEqual(result.returncode, 0, result.stdout)
        for rel, content in self.pinned.items():
            self.assertEqual((self.vendor / rel).read_bytes(), content, rel)

    def test_patch_conflict_leaves_vendor_untouched(self):
        path = self.up / 'Development/HIP/hip_d3d12_bridge.h'
        write(path, path.read_text().replace('HANDLE fence_handle', 'HANDLE moved_fence_handle'))
        commit(self.up)
        before = (self.vendor / 'shaders/active.hlsl').read_bytes()
        result = self.sync(('-UpdateBridge',))
        self.assert_failed(result)
        self.assertIn('bridge.patch', result.stdout)
        self.assertEqual((self.vendor / 'shaders/active.hlsl').read_bytes(), before)
        self.assertEqual((self.vendor / 'Development/HIP/hip_d3d12_bridge.h').read_bytes(), self.pinned['Development/HIP/hip_d3d12_bridge.h'])

    def test_audit_nonzero_never_advances_pin(self):
        result = self.sync(audit_exit=19)
        self.assert_failed(result)
        self.assertIn('exit 19', result.stdout)
        self.assertEqual(audit.read_json(self.vendor / 'sync-state.json')['status'], 'pending')

    def test_missing_audit_is_fatal_before_copy(self):
        (self.local / 'tools/audit-lmxxf-enablements.py').unlink()
        self.assert_failed(self.sync(write_audit=False))
        self.assertFalse((self.vendor / 'sync-state.json').exists())

    def test_unknown_module_baseline_requires_rebuild_or_explicit_waiver(self):
        result = self.sync(allow_stale=False)
        self.assert_failed(result)
        self.assertIn('HIP recipes/defines changed', result.stdout)

    def test_missing_python_is_fatal_before_copy(self):
        result = self.sync(python=str(self.folder / 'missing-python.exe'))
        self.assert_failed(result)
        self.assertFalse((self.vendor / 'sync-state.json').exists())

    def test_explicit_audit_skip_is_source_only(self):
        result = self.sync(('-SkipEnablementAudit',))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn('Source sync and integration review complete:', result.stdout)
        self.assertEqual((self.vendor / 'UPSTREAM.md').read_bytes(), self.pin_before)
        self.assertEqual(audit.read_json(self.vendor / 'sync-state.json')['status'], 'pending')
        self.assertFalse((self.local / 'tools/audit-invocation.json').exists())

    def test_retry_cannot_forget_recipe_change(self):
        write(self.up / 'hip/active.hip', '// changed recipe\n')
        commit(self.up)
        self.assert_failed(self.sync(audit_exit=3, allow_stale=False))
        result = self.sync(allow_stale=False)
        self.assert_failed(result)
        self.assertIn('HIP recipes/defines changed', result.stdout)

    def test_runtime_build_failure_does_not_complete(self):
        write(self.local / 'tools/build-lmxxf-runtime.cmd', '@exit /b 23\n')
        result = self.sync(skip_build=False)
        self.assert_failed(result)
        self.assertIn('exit 23', result.stdout)

    def test_missing_runtime_builder_is_fatal(self):
        (self.local / 'tools/build-lmxxf-runtime.cmd').unlink()
        self.assert_failed(self.sync(skip_build=False))
        self.assertFalse((self.vendor / 'sync-state.json').exists())

    def test_hip_rename_removes_old_source(self):
        (self.up / 'hip/active.hip').rename(self.up / 'hip/renamed.hip')
        path = self.up / 'hip/build-modules.ps1'
        write(path, path.read_text().replace('active.hip', 'renamed.hip'))
        commit(self.up)
        result = self.sync()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse((self.vendor / 'hip/active.hip').exists())
        self.assertTrue((self.vendor / 'hip/renamed.hip').is_file())


class AuditTests(Fixture):
    def collect(self, skipped=()):
        return audit.collect(self.local, audit.Git(self.up), self.base,
            git(self.up, 'rev-parse', 'HEAD'), skipped)[0]

    def reviewed(self, report):
        # Synthetic validator input only; not an approval of the real source snapshot.
        review = audit.template(report)
        review.update(reviewer='fixture', summary='fixture-only decision', validation='fixture-only validation')
        for entry in review['decisions']:
            entry.update(classification='experiment', decision='deferred', reason='fixture reason',
                evidence='fixture source', next_step='run fixture validation')
        return review

    def test_records_disabled_numeric_unknown_and_per_module_gates(self):
        overrides = audit.read_json(self.config / 'module-defines.json')
        del overrides['multihead-fast-padded-wave']
        write(self.config / 'module-defines.json', json.dumps(overrides))
        items = {item['id']: item for item in self.collect()['items']}
        self.assertEqual(items['flag:DLSS5_HIP_GRAPH']['evidence']['profiles'][0]['value'], '0')
        self.assertEqual(items['flag:DLSS5_NETWORK_HEIGHT']['evidence']['profiles'][0]['value'], '900')
        for key in ('flag:DLSS5_TEST_UNKNOWN', 'kernel:HIP_EXPERIMENT', 'kernel:HIP_OTHER'):
            self.assertIn(key, items)
        uses = items['kernel:HIP_FFN_LINE_STORES']['evidence']['per_module']
        by_module = {use['module']: use['recipe_and_local_defines'] for use in uses}
        self.assertEqual(by_module['multihead-fast-padded-wave'], [])
        self.assertEqual(by_module['multihead-fast-padded-wave-packed'], ['HIP_FFN_LINE_STORES 1'])

    def test_missing_profile_and_recipe_source_are_errors(self):
        (self.up / audit.PROFILES[0]).unlink()
        commit(self.up)
        with self.assertRaises(RuntimeError):
            self.collect()
        write(self.up / audit.PROFILES[0], 'DLSS5_HIP_GRAPH=0\n')
        (self.up / 'hip/active.hip').unlink()
        commit(self.up)
        with self.assertRaises(RuntimeError):
            self.collect()

    def test_template_not_approval_and_deferral_needs_next_step(self):
        report = self.collect()
        self.assertTrue(audit.validate_review(report, audit.template(report)))
        reviewed = self.reviewed(report)
        self.assertEqual(audit.validate_review(report, reviewed), [])
        reviewed['decisions'][0]['next_step'] = ''
        self.assertTrue(audit.validate_review(report, reviewed))

    def test_local_changes_and_unvendored_upstream_changes_invalidate_review(self):
        report = self.collect()
        reviewed = self.reviewed(report)
        path = self.local / audit.OPTIONS
        write(path, path.read_text().replace('o.graph = false', 'o.graph = true'))
        self.assertTrue(audit.validate_review(self.collect(), reviewed))
        write(self.up / 'scripts/new-profile.txt', 'DLSS5_NEW_FEATURE=1\n')
        commit(self.up)
        newer = self.collect()
        self.assertIn('file:scripts/new-profile.txt', {item['id'] for item in newer['items']})
        self.assertTrue(audit.validate_review(newer, reviewed))

    def test_integrated_needs_validation_and_duplicate_ids_fail(self):
        report = self.collect()
        reviewed = self.reviewed(report)
        reviewed['decisions'][0]['decision'] = 'integrated'
        self.assertTrue(audit.validate_review(report, reviewed))
        reviewed['decisions'][0]['validation'] = 'fixture validation passed'
        self.assertEqual(audit.validate_review(report, reviewed), [])
        reviewed['decisions'].append(copy.deepcopy(reviewed['decisions'][0]))
        self.assertTrue(audit.validate_review(report, reviewed))

    def test_only_unchanged_decisions_are_carried(self):
        report = self.collect()
        reviewed = self.reviewed(report)
        path = self.local / audit.OPTIONS
        write(path, path.read_text() + '\n// changed integration\n')
        carried = audit.template(self.collect(), reviewed)
        self.assertEqual(carried['decisions'][0]['decision'], 'pending')
        self.assertEqual(carried['validation'], '')
        self.assertTrue(any(entry['decision'] == 'deferred' for entry in carried['decisions']))

    def test_external_modules_are_bound_to_review_content(self):
        bundle = self.folder / 'external modules'
        bundle.mkdir()
        (bundle / 'test.hsaco').write_bytes(b'first binary')
        repository = audit.Git(self.up)
        report = audit.collect(self.local, repository, self.base, self.target, [], bundle)[0]
        reviewed = self.reviewed(report)
        (bundle / 'test.hsaco').write_bytes(b'different binary')
        changed = audit.collect(self.local, repository, self.base, self.target, [], bundle)[0]
        self.assertTrue(audit.validate_review(changed, reviewed))

    def test_external_modules_dual_arch_detects_gfx1200_change(self):
        bundle = self.folder / 'external modules dual'
        bundle.mkdir()
        (bundle / 'gfx1200').mkdir()
        (bundle / 'gfx1201').mkdir()
        (bundle / 'gfx1200/test.hsaco').write_bytes(b'gfx1200 binary')
        (bundle / 'gfx1201/test.hsaco').write_bytes(b'gfx1201 binary')
        write(bundle / 'gfx1200/SHA256SUMS', audit.file_hash(bundle / 'gfx1200/test.hsaco') + '  test.hsaco\n')
        write(bundle / 'gfx1201/SHA256SUMS', audit.file_hash(bundle / 'gfx1201/test.hsaco') + '  test.hsaco\n')
        repository = audit.Git(self.up)
        report = audit.collect(self.local, repository, self.base, self.target, [], bundle)[0]
        items = {item['id']: item for item in report['items']}
        self.assertIn('modules:external', items)
        ext = items['modules:external']['evidence']
        self.assertIn('gfx1200/test.hsaco', ext['binaries'])
        self.assertIn('gfx1201/test.hsaco', ext['binaries'])
        self.assertIn('gfx1200/SHA256SUMS', ext['metadata'])
        self.assertIn('gfx1201/SHA256SUMS', ext['metadata'])
        reviewed = self.reviewed(report)
        (bundle / 'gfx1200/test.hsaco').write_bytes(b'gfx1200 modified')
        changed = audit.collect(self.local, repository, self.base, self.target, [], bundle)[0]
        self.assertTrue(audit.validate_review(changed, reviewed))

    def test_text_fingerprint_survives_checkout_line_endings(self):
        path = self.folder / 'text.txt'
        path.write_bytes(b'first\nsecond\n')
        expected = audit.file_hash(path)
        path.write_bytes(b'\xef\xbb\xbffirst\r\nsecond\r\n')
        self.assertEqual(audit.file_hash(path), expected)

    def test_inventory_includes_false_chains_and_values(self):
        actual = audit.assignments('o.a = o.b = true; o.c=false; o.height = 900; /*o.fake=true;*/ o.mode = choose();')
        self.assertEqual(actual, {'a':['true'], 'b':['true'], 'c':['false'], 'height':['900'], 'mode':['choose()']})


@unittest.skipUnless(PS and os.name == 'nt', 'PowerShell/Windows required')
class ModuleTests(Fixture):
    def helpers(self, body):
        runner = self.local / 'helpers-test.ps1'
        write(runner, "$ErrorActionPreference = 'Stop'\n"
            ". (Join-Path $env:LMXXF_FIXTURE_CONFIG 'Files.ps1')\n"
            ". (Join-Path $env:LMXXF_FIXTURE_CONFIG 'Modules.ps1')\n"
            "$hip = Join-Path $env:LMXXF_FIXTURE_VENDOR 'hip'\n"
            "$modules = Join-Path $env:LMXXF_FIXTURE_VENDOR 'modules'\n" + body)
        env = os.environ.copy()
        env['LMXXF_FIXTURE_CONFIG'] = str(self.config)
        env['LMXXF_FIXTURE_VENDOR'] = str(self.vendor)
        env['LMXXF_FIXTURE_UPSTREAM'] = str(self.up)
        return subprocess.run([PS, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(runner)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, encoding='utf-8', errors='replace',
            env=env, timeout=60)

    def mock_recipe(self, exit_code=0, conflicting=False):
        path = self.vendor / 'hip/build-modules.ps1'
        recipe = path.read_text()
        if conflicting:
            recipe = recipe.replace('defines = @()', "defines = @('HIP_FFN_LINE_STORES 0')")
        write(path, 'param($OutputDir, $Compiler, $SourceDir, $Targets)\n' + recipe + '''
foreach ($module in $modules) {
    [IO.File]::WriteAllText((Join-Path $OutputDir ($module.name + '.hsaco')), ($module.defines -join '|'))
    Write-Output 'fixture hash output'
}
''' + 'exit ' + str(exit_code) + '\n')
        return '''
function cl.exe {
    foreach ($arg in $args) {
        if ($arg.StartsWith('/Fe:')) { [IO.File]::WriteAllText($arg.Substring(4), 'fixture compiler') }
    }
    Write-Output 'fixture compiler banner'
    $global:LASTEXITCODE = 0
}
$result = Invoke-BuildGfx1201Modules $hip (Join-Path $hip '_build_gfx1201')
if ($result -isnot [string] -or -not (Test-Path -LiteralPath $result -PathType Container)) {
    throw 'Compiler/recipe output polluted the module-directory return value'
}
'''

    def test_build_returns_one_path_and_injects_each_module(self):
        result = self.helpers(self.mock_recipe())
        self.assertEqual(result.returncode, 0, result.stdout)
        for name in ('multihead-fast-padded-wave', 'multihead-fast-padded-wave-packed'):
            text = (self.vendor / 'hip/_build_gfx1201' / (name + '.hsaco')).read_text()
            self.assertEqual(text, 'HIP_FFN_LINE_STORES 1')

    def test_recipe_exit_and_conflicting_override_fail(self):
        result = self.helpers(self.mock_recipe(exit_code=27))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('27', result.stdout)

    def test_conflicting_upstream_override_fails(self):
        result = self.helpers(self.mock_recipe(conflicting=True))
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('differently', result.stdout)

    def test_corrupt_bundle_fails_before_destination_copy(self):
        bundle = self.up / 'modules'
        bundle.mkdir()
        (bundle / 'test.hsaco').write_bytes(b'corrupt')
        write(bundle / 'SHA256SUMS', '0' * 64 + '  test.hsaco\n')
        before = (self.vendor / 'modules/test.hsaco').read_bytes()
        result = self.helpers("Sync-LmxxfModules (Join-Path $env:LMXXF_FIXTURE_UPSTREAM 'modules') $modules 'fixture'\n")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('checksum mismatch', result.stdout)
        self.assertEqual((self.vendor / 'modules/test.hsaco').read_bytes(), before)

    def test_valid_bundle_removes_retired_modules_and_rehashes(self):
        bundle = self.up / 'modules'
        bundle.mkdir()
        data = b'new module'
        (bundle / 'new.hsaco').write_bytes(data)
        expected = hashlib.sha256(data).hexdigest()
        write(bundle / 'SHA256SUMS', expected + '  new.hsaco\n')
        write(self.vendor / 'modules/notes.txt', 'local metadata')
        result = self.helpers("Sync-LmxxfModules (Join-Path $env:LMXXF_FIXTURE_UPSTREAM 'modules') $modules 'fixture'\n")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertFalse((self.vendor / 'modules/test.hsaco').exists())
        self.assertEqual((self.vendor / 'modules/new.hsaco').read_bytes(), data)
        self.assertTrue((self.vendor / 'modules/notes.txt').exists())
        self.assertIn(expected, (self.vendor / 'modules/SHA256SUMS').read_text())

    def test_fingerprint_detects_gfx1200_change(self):
        bundle = self.folder / 'fingerprint-modules'
        bundle.mkdir()
        (bundle / 'gfx1200').mkdir()
        (bundle / 'gfx1201').mkdir()
        (bundle / 'gfx1200/test.hsaco').write_bytes(b'gfx1200 v1')
        (bundle / 'gfx1201/test.hsaco').write_bytes(b'gfx1201 v1')
        write(bundle / 'gfx1200/SHA256SUMS', hashlib.sha256(b'gfx1200 v1').hexdigest() + '  test.hsaco\n')
        write(bundle / 'gfx1201/SHA256SUMS', hashlib.sha256(b'gfx1201 v1').hexdigest() + '  test.hsaco\n')
        fp1_res = self.helpers("Get-TreeFingerprint '" + str(bundle).replace('\\', '/') + "'\n")
        self.assertEqual(fp1_res.returncode, 0, fp1_res.stdout)
        fp1 = fp1_res.stdout.strip()
        # Modify only gfx1200
        (bundle / 'gfx1200/test.hsaco').write_bytes(b'gfx1200 v2')
        fp2_res = self.helpers("Get-TreeFingerprint '" + str(bundle).replace('\\', '/') + "'\n")
        self.assertEqual(fp2_res.returncode, 0, fp2_res.stdout)
        fp2 = fp2_res.stdout.strip()
        self.assertNotEqual(fp1, fp2)

    def test_dual_arch_bundle_sync_and_validation(self):
        bundle = self.up / 'dual_modules'
        bundle.mkdir()
        (bundle / 'gfx1200').mkdir()
        (bundle / 'gfx1201').mkdir()
        data_1200 = b'gfx1200 code'
        data_1201 = b'gfx1201 code'
        (bundle / 'gfx1200/m1200.hsaco').write_bytes(data_1200)
        (bundle / 'gfx1201/m1201.hsaco').write_bytes(data_1201)
        h1200 = hashlib.sha256(data_1200).hexdigest()
        h1201 = hashlib.sha256(data_1201).hexdigest()
        write(bundle / 'gfx1200/SHA256SUMS', f'{h1200}  m1200.hsaco\n')
        write(bundle / 'gfx1201/SHA256SUMS', f'{h1201}  m1201.hsaco\n')
        write(bundle / 'SHA256SUMS', f'{h1200}  gfx1200/m1200.hsaco\n{h1201}  gfx1201/m1201.hsaco\n')
        write(bundle / 'modules.json', '{"targets": ["gfx1200", "gfx1201"]}')
        write(self.vendor / 'modules/notes.txt', 'local metadata')
        
        result = self.helpers("Sync-LmxxfModules '" + str(bundle).replace('\\', '/') + "' $modules 'fixture'\n")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual((self.vendor / 'modules/gfx1200/m1200.hsaco').read_bytes(), data_1200)
        self.assertEqual((self.vendor / 'modules/gfx1201/m1201.hsaco').read_bytes(), data_1201)
        self.assertTrue((self.vendor / 'modules/notes.txt').exists())
        sums_text = (self.vendor / 'modules/SHA256SUMS').read_text()
        self.assertIn(f'{h1200}  gfx1200/m1200.hsaco', sums_text)
        self.assertIn(f'{h1201}  gfx1201/m1201.hsaco', sums_text)

    def test_corrupt_dual_arch_bundle_fails_before_destination_copy(self):
        bundle = self.up / 'bad_dual_modules'
        bundle.mkdir()
        (bundle / 'gfx1200').mkdir()
        (bundle / 'gfx1201').mkdir()
        (bundle / 'gfx1200/m1200.hsaco').write_bytes(b'good')
        (bundle / 'gfx1201/m1201.hsaco').write_bytes(b'corrupted')
        h1200 = hashlib.sha256(b'good').hexdigest()
        h1201 = hashlib.sha256(b'different').hexdigest()
        write(bundle / 'gfx1200/SHA256SUMS', f'{h1200}  m1200.hsaco\n')
        write(bundle / 'gfx1201/SHA256SUMS', f'{h1201}  m1201.hsaco\n')
        write(bundle / 'SHA256SUMS', f'{h1200}  gfx1200/m1200.hsaco\n{h1201}  gfx1201/m1201.hsaco\n')
        
        before = (self.vendor / 'modules/test.hsaco').read_bytes()
        result = self.helpers("Sync-LmxxfModules '" + str(bundle).replace('\\', '/') + "' $modules 'fixture'\n")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('checksum mismatch', result.stdout)
        self.assertEqual((self.vendor / 'modules/test.hsaco').read_bytes(), before)
        self.assertFalse((self.vendor / 'modules/gfx1200').exists())

    def test_uninstall_dual_arch_modules_preserves_user_files(self):
        game = self.folder / 'game_for_uninstall'
        game.mkdir()
        mods = game / 'lmxxf-modules'
        mods.mkdir()
        g1200 = mods / 'gfx1200'
        g1201 = mods / 'gfx1201'
        g1200.mkdir()
        g1201.mkdir()

        # Installer files
        (g1200 / 'k1.hsaco').write_bytes(b'k1 code')
        (g1201 / 'k2.hsaco').write_bytes(b'k2 code')
        h1 = hashlib.sha256(b'k1 code').hexdigest()
        h2 = hashlib.sha256(b'k2 code').hexdigest()
        write(g1200 / 'SHA256SUMS', f'{h1}  k1.hsaco\n')
        write(g1201 / 'SHA256SUMS', f'{h2}  k2.hsaco\n')
        write(mods / 'SHA256SUMS', f'{h1}  gfx1200/k1.hsaco\n{h2}  gfx1201/k2.hsaco\n')
        write(mods / 'modules.json', '{}')
        write(g1200 / 'modules.json', '{}')

        # User files that must NOT be removed
        user_file_arch = g1200 / 'user_weights.bin'
        user_file_arch.write_bytes(b'weights')
        user_file_root = mods / 'user_config.bin'
        user_file_root.write_bytes(b'config')

        script = ROOT / 'tools/uninstall-amd-presr.ps1'
        cmd = [PS, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script),
               '-GameDir', str(game), '-NonInteractive']
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

        # Installer files deleted
        self.assertFalse((g1200 / 'k1.hsaco').exists())
        self.assertFalse((g1201 / 'k2.hsaco').exists())
        self.assertFalse((g1201 / 'SHA256SUMS').exists())
        self.assertFalse((mods / 'SHA256SUMS').exists())
        self.assertFalse((mods / 'modules.json').exists())
        # Empty arch dir removed
        self.assertFalse(g1201.exists())

        # User files and containing dirs preserved
        self.assertTrue(user_file_arch.exists())
        self.assertTrue(user_file_root.exists())
        self.assertTrue(g1200.exists())
        self.assertTrue(mods.exists())


class AuditCliTests(Fixture):
    def test_real_cli_requires_review_and_rejects_stale_local_code(self):
        command = [sys.executable, str(self.local / 'tools/audit-lmxxf-enablements.py'),
            str(self.up), self.target, '--base', self.base]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        report = audit.read_json(self.local / 'exports/lmxxf-upstream/report.json')
        review = audit.template(report)
        review.update(reviewer='fixture only', summary='fixture plan', validation='fixture validation')
        for entry in review['decisions']:
            entry.update(classification='experiment', decision='deferred', reason='fixture',
                evidence='fixture source', next_step='fixture follow-up')
        write(self.vendor / 'upstream-review.json', json.dumps(review))
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        path = self.local / audit.OPTIONS
        write(path, path.read_text() + '\n// changed local integration\n')
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main(verbosity=2)
