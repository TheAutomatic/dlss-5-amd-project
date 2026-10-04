"""Validate reuse against actual source/artifact changes, never just file dates."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('manifest', ROOT / 'tools/build/mochizuki-manifest.py')
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)


class BuildCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.patch = patch.object(manifest, 'ROOT', self.root); self.patch.start(); self.addCleanup(self.patch.stop)
        self.source = self.root / 'third_party/mochizuki/windows/test.cpp'
        product = 'OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/'
        paths = [self.source, *(self.root / (product + p) for p in (
            'dlssnr/backend/lmxxf_runtime/LmxxfNrApi.h', 'dlssnr/NrPerformance.h', 'library/vulkan/vulkan-1.lib')),
            *(self.root / 'tools/build' / f for f in ('build-mochizuki-runtime.py',
              'build-mochizuki-runtime.cmd', 'mochizuki-deps.py', 'mochizuki-manifest.py'))]
        for p in paths:
            p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b'fixture source')
        self.out = self.root / 'exports/mochizuki-runtime'
        self.shader = self.out / 'dlssnr-amd/shaders/test.spv'
        self.shader.parent.mkdir(parents=True); self.shader.write_bytes(b'fixture shader')
        self.dll = self.out / 'MochizukiNrRuntime.dll'; self.dll.write_bytes(b'fixture DLL')
        manifest.write(self.out)

    def test_identical_build_reuses(self):
        self.assertTrue(manifest.reusable(self.out))

    def test_source_addition_change_and_toolchain_invalidate(self):
        self.source.write_bytes(b'changed'); self.assertFalse(manifest.reusable(self.out))
        self.source.write_bytes(b'fixture source')
        with patch.dict(os.environ, {'VCToolsVersion': 'different'}):
            self.assertFalse(manifest.reusable(self.out))
        new = self.source.with_name('new.h'); new.write_bytes(b'new include')
        self.assertFalse(manifest.reusable(self.out))

    def test_missing_or_changed_shader_and_dll_invalidate(self):
        self.shader.unlink(); self.assertFalse(manifest.reusable(self.out))
        self.shader.write_bytes(b'changed shader'); self.assertFalse(manifest.reusable(self.out))
        self.shader.write_bytes(b'fixture shader'); self.dll.write_bytes(b'changed DLL')
        self.assertFalse(manifest.reusable(self.out))

    def test_missing_corrupt_or_legacy_manifest_rebuilds(self):
        path = self.out / 'build-manifest.json'
        for content in ('{broken', '{}', '[]'):
            path.write_text(content); self.assertFalse(manifest.reusable(self.out))
        path.unlink(); self.assertFalse(manifest.reusable(self.out))


if __name__ == '__main__':
    unittest.main()
