"""Validate actual LLVM source/defines/options against copied module metadata."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PS = os.environ.get('LMXXF_TEST_POWERSHELL', shutil.which('powershell.exe') or shutil.which('pwsh'))


@unittest.skipUnless(PS, 'PowerShell required')
class CompilerProvenanceTests(unittest.TestCase):
    def fixture(self, folder):
        out, pre = folder / 'out', folder / 'pre'
        pre.mkdir()
        recipe = folder / 'recipe.ps1'
        recipe.write_text("@{ name = 'c64-wave2'; defines = @('W2_PACK8 6'); sources = @('a.hip'); compiler = 'llvm23'; l23defines = @('HIP_BARRIER_FENCE 1') }\n")
        source = '#define HIP_ISA_HALF 1\n#define W2_PACK8 6\nvoid kernel() {}\n'
        compiled = source.replace('void kernel', '#define HIP_BARRIER_FENCE 1\nvoid kernel')
        rows = []
        for arch in ('gfx1200', 'gfx1201'):
            leaf = out / arch
            leaf.mkdir(parents=True)
            (leaf / 'c64-wave2.generated.hip').write_text(source)
            digest = hashlib.sha256(b'module-' + arch.encode()).hexdigest()
            (leaf / 'modules.json').write_text(json.dumps([dict(target=arch, module='c64-wave2',
                defines='HIP_ISA_HALF 1; W2_PACK8 6', opts='llvm23 prebuilt ', sha256=digest)]))
            rows.append(dict(target=arch, module='c64-wave2', sha256=digest,
                source_sha256=hashlib.sha256(compiled.encode()).hexdigest(),
                defines=['HIP_ISA_HALF 1', 'W2_PACK8 6', 'HIP_BARRIER_FENCE 1'],
                commands=[['clang', '-real-true16'], ['clang', '-real-true16'], ['clang'], ['ld.lld']]))
        manifest = dict(compiler='clang version 23.1.2', modules=rows)
        return out, pre, recipe, manifest

    def invoke(self, folder, out, pre, recipe, manifest):
        (pre / 'manifest.json').write_text(json.dumps(manifest))
        runner = folder / 'run.ps1'
        helper = str(ROOT / 'tools/lmxxf-sync/Modules.ps1').replace("'", "''")
        runner.write_text("$ErrorActionPreference='Stop'\n. '" + helper + "'\n"
            + "Complete-LmxxfCompilerProvenance '" + str(out) + "' '" + str(recipe)
            + "' '" + str(pre) + "' @('gfx1200','gfx1201')\n")
        return subprocess.run([PS, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(runner)],
            capture_output=True, text=True, errors='replace', timeout=60)

    def test_c64_fence_is_recorded_and_source_is_bound(self):
        with tempfile.TemporaryDirectory(prefix='lmxxf compiler provenance ') as folder:
            root = Path(folder)
            out, pre, recipe, manifest = self.fixture(root)
            result = self.invoke(root, out, pre, recipe, manifest)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for arch in ('gfx1200', 'gfx1201'):
                entry = json.loads((out / arch / 'modules.json').read_text())[0]
                self.assertIn('HIP_BARRIER_FENCE 1', entry['defines'])
                self.assertEqual(entry['source_sha256'], manifest['modules'][0]['source_sha256'])

    def test_wrong_source_fence_object_or_target_feature_is_rejected(self):
        for failure in ('source', 'fence', 'object', 'target-feature'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory(prefix='lmxxf compiler provenance ') as folder:
                root = Path(folder)
                out, pre, recipe, manifest = self.fixture(root)
                row = manifest['modules'][0]
                if failure == 'source': row['source_sha256'] = '0' * 64
                elif failure == 'fence': row['defines'].pop()
                elif failure == 'object': row['sha256'] = '0' * 64
                else: row['commands'][1] = ['clang']
                result = self.invoke(root, out, pre, recipe, manifest)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
