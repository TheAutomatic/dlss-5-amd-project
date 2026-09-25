import os
import shutil
import tempfile
import unittest
import ctypes
from pathlib import Path
import subprocess

class LmxxfNrCreateInfo(ctypes.Structure):
    _fields_ = [
        ('struct_size', ctypes.c_uint32),
        ('device', ctypes.c_void_p),
        ('queue', ctypes.c_void_p),
        ('assets_directory', ctypes.c_wchar_p),
        ('flags', ctypes.c_uint32),
    ]

class LmxxfNrApi(ctypes.Structure):
    pass

LmxxfNrApi._fields_ = [
    ('struct_size', ctypes.c_uint32),
    ('abi_version', ctypes.c_uint32),
    ('QueryCapabilities', ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_void_p)),
    ('Create', ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.POINTER(LmxxfNrCreateInfo), ctypes.POINTER(ctypes.c_void_p))),
    ('Destroy', ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_void_p)),
    ('PrepareSession', ctypes.c_void_p),
    ('PrepareFrame', ctypes.c_void_p),
    ('RecordInputs', ctypes.c_void_p),
    ('EnqueueHip', ctypes.c_void_p),
    ('RecordOutputs', ctypes.c_void_p),
    ('ExecuteAfterProducer', ctypes.c_void_p),
    ('CancelUnsubmitted', ctypes.c_void_p),
    ('Poll', ctypes.c_void_p),
    ('Retire', ctypes.c_void_p),
    ('ResetHistory', ctypes.c_void_p),
    ('Drain', ctypes.c_void_p),
    ('GetStatus', ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32)),
    ('GetLastError', ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_char_p, ctypes.c_uint32)),
]

class RuntimeValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        dll_path = os.path.abspath(os.environ.get('LMXXF_TEST_RUNTIME', r'exports\lmxxf-runtime\LmxxfNrRuntime.dll'))
        assert os.path.exists(dll_path), f"DLL not found: {dll_path}"
        cls.dll = ctypes.WinDLL(dll_path)

        cls.get_api = cls.dll.LmxxfNrGetApi
        cls.get_api.argtypes = [ctypes.c_uint32, ctypes.POINTER(LmxxfNrApi)]
        cls.get_api.restype = ctypes.c_int32

        cls.api = LmxxfNrApi()
        cls.api.struct_size = ctypes.sizeof(LmxxfNrApi)
        rc = cls.get_api(1, ctypes.byref(cls.api))
        assert rc == 0, f"GetApi failed: {rc}"

        cls.resolve_arch = cls.dll.LmxxfNrResolveArchModules
        cls.resolve_arch.argtypes = [
            ctypes.c_wchar_p, ctypes.c_char_p,
            ctypes.c_wchar_p, ctypes.c_uint32,
            ctypes.c_char_p, ctypes.c_uint32
        ]
        cls.resolve_arch.restype = ctypes.c_int32

        cls.real_modules = os.path.abspath(os.environ.get('LMXXF_TEST_MODULES', r'third_party\lmxxf\modules'))

    def call_create(self, modules_dir):
        ctx = ctypes.c_void_p()
        info = LmxxfNrCreateInfo()
        info.struct_size = ctypes.sizeof(LmxxfNrCreateInfo)
        info.device = 0x100
        info.queue = 0x200
        info.assets_directory = modules_dir
        rc = self.api.Create(ctypes.byref(info), ctypes.byref(ctx))
        err_buf = ctypes.create_string_buffer(512)
        self.api.GetLastError(err_buf, 512)
        err = err_buf.value.decode('utf-8', errors='replace')
        return rc, ctx, err

    def test_valid_dual_arch_modules(self):
        rc, ctx, err = self.call_create(self.real_modules)
        self.assertEqual(rc, 0, f"Create failed: {err}")
        self.assertIsNotNone(ctx.value)

        status_buf = ctypes.create_string_buffer(256)
        self.api.GetStatus(ctx, status_buf, 256)
        st = status_buf.value.decode()
        self.assertIn("modules_ok=48", st)
        self.assertIn("arch=unknown", st)
        self.assertIn("pdl=0/0(unknown)", st)
        self.assertIn("hip=0", st)

        self.assertEqual(self.api.Destroy(ctx), 0)

    def test_valid_flat_leaf_modules(self):
        leaf = os.path.join(self.real_modules, "gfx1201")
        rc, ctx, err = self.call_create(leaf)
        self.assertEqual(rc, 0, f"Create failed: {err}")
        self.assertIsNotNone(ctx.value)

        status_buf = ctypes.create_string_buffer(256)
        self.api.GetStatus(ctx, status_buf, 256)
        st = status_buf.value.decode()
        self.assertIn("modules_ok=24", st)
        self.assertIn("arch=unknown", st)
        self.assertIn("pdl=0/0(unknown)", st)
        self.assertIn("hip=0", st)

        self.assertEqual(self.api.Destroy(ctx), 0)

    def test_equivalent_trailing_separators(self):
        for directory in (self.real_modules, os.path.join(self.real_modules, 'gfx1201')):
            for separator in ('\\', '/', '\\\\'):
                with self.subTest(directory=directory, separator=separator):
                    rc, ctx, err = self.call_create(directory + separator)
                    try:
                        self.assertEqual(rc, 0, err)
                        self.assertIsNotNone(ctx.value)
                    finally:
                        if ctx.value:
                            self.api.Destroy(ctx)

    def test_relative_path_with_trailing_separator(self):
        relative = os.path.relpath(self.real_modules) + os.sep
        rc, ctx, err = self.call_create(relative)
        try:
            self.assertEqual(rc, 0, err)
        finally:
            if ctx.value:
                self.api.Destroy(ctx)

    def test_extended_length_module_path(self):
        with tempfile.TemporaryDirectory(prefix='lmxxf-runtime-long-') as td:
            nested = Path(td) / ('a' * 90) / ('b' * 90) / ('c' * 90)
            extended = '\\\\?\\' + str(nested)
            try:
                shutil.copytree(self.real_modules, extended)
                self.assertGreater(len(extended), 260)
                rc, ctx, err = self.call_create(extended + '\\')
                try:
                    self.assertEqual(rc, 0, err)
                finally:
                    if ctx.value:
                        self.api.Destroy(ctx)
            finally:
                # Use the same extended path for cleanup; the short spelling can
                # exceed Win32 MAX_PATH while traversing this intentionally long tree.
                self.assertEqual(os.path.commonpath((str(nested), td)), td)
                if os.path.exists(extended):
                    shutil.rmtree(extended)

    def make_junction(self, path, target):
        env = os.environ.copy()
        env['LMXXF_TEST_LINK'] = str(path)
        env['LMXXF_TEST_TARGET'] = str(target)
        result = subprocess.run(['powershell.exe', '-NoProfile', '-Command',
            'New-Item -ItemType Junction -Path $env:LMXXF_TEST_LINK -Target $env:LMXXF_TEST_TARGET | Out-Null'],
            env=env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_architecture_junction_cannot_escape_module_root(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / 'modules'
            outside = Path(td) / 'outside'
            shutil.copytree(self.real_modules, root)
            (root / 'gfx1200').rename(outside)
            self.make_junction(root / 'gfx1200', outside)
            try:
                rc, ctx, err = self.call_create(str(root))
                self.assertEqual(rc, 4, err)
                self.assertIsNone(ctx.value)
                self.assertIn('reparse point', err)
            finally:
                os.rmdir(root / 'gfx1200')
            self.assertTrue((outside / 'c32_fast.hsaco').exists())

    def test_module_root_junction_is_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td) / 'modules'
            outside = Path(td) / 'outside'
            shutil.copytree(self.real_modules, outside)
            self.make_junction(root, outside)
            try:
                rc, ctx, err = self.call_create(str(root))
                self.assertEqual(rc, 4, err)
                self.assertIsNone(ctx.value)
                self.assertIn('reparse point', err)
            finally:
                os.rmdir(root)

    def test_manifest_symlinks_are_rejected(self):
        for relative in ('SHA256SUMS', 'runtime-manifest.json', 'gfx1200/SHA256SUMS', 'gfx1201/modules.json'):
            with self.subTest(relative=relative), tempfile.TemporaryDirectory() as td:
                root = Path(td) / 'modules'
                outside = Path(td) / 'outside-manifest'
                shutil.copytree(self.real_modules, root)
                path = root / relative
                path.rename(outside)
                try:
                    path.symlink_to(outside)
                except OSError as exc:
                    if getattr(exc, 'winerror', None) == 1314:
                        self.skipTest('Creating file symlinks requires Windows Developer Mode or privilege')
                    raise
                try:
                    rc, ctx, err = self.call_create(str(root))
                    self.assertEqual(rc, 4, err)
                    self.assertIsNone(ctx.value)
                    self.assertIn('reparse point', err)
                finally:
                    path.unlink()

    def test_v1_abi_capabilities_and_pdl_status(self):
        class LmxxfNrCapabilities(ctypes.Structure):
            _fields_ = [
                ("struct_size", ctypes.c_uint32),
                ("abi_version", ctypes.c_uint32),
                ("max_input_width", ctypes.c_uint32),
                ("max_input_height", ctypes.c_uint32),
                ("history_supported", ctypes.c_uint32),
                ("overlap_supported", ctypes.c_uint32),
                ("graph_supported", ctypes.c_uint32),
                ("gfx1201_target", ctypes.c_uint32),
            ]
        caps = LmxxfNrCapabilities()
        caps.struct_size = ctypes.sizeof(LmxxfNrCapabilities)
        rc = self.api.QueryCapabilities(ctypes.byref(caps))
        self.assertEqual(rc, 0)
        self.assertEqual(caps.abi_version, 1)
        self.assertEqual(caps.history_supported, 0)
        self.assertEqual(caps.overlap_supported, 0)
        self.assertEqual(caps.graph_supported, 0)
        self.assertEqual(caps.gfx1201_target, 1)  # preserved deprecated v1 target

    def test_resolve_arch_modules(self):
        out = ctypes.create_unicode_buffer(260)
        err = ctypes.create_string_buffer(256)

        # 1. Dual-arch root resolves gfx1200 and gfx1201
        rc = self.resolve_arch(self.real_modules, b"gfx1200", out, 260, err, 256)
        self.assertEqual(rc, 0)
        self.assertTrue(out.value.endswith("gfx1200"))

        rc = self.resolve_arch(self.real_modules, b"gfx1201", out, 260, err, 256)
        self.assertEqual(rc, 0)
        self.assertTrue(out.value.endswith("gfx1201"))

        # 2. Unsupported arch
        rc = self.resolve_arch(self.real_modules, b"gfx1100", out, 260, err, 256)
        self.assertEqual(rc, 4)
        self.assertIn("unsupported HIP architecture", err.value.decode())

        # 3. Flat leaf resolves to itself
        leaf = os.path.join(self.real_modules, "gfx1201")
        rc = self.resolve_arch(leaf, b"gfx1201", out, 260, err, 256)
        self.assertEqual(rc, 0)
        self.assertEqual(os.path.normpath(out.value), os.path.normpath(leaf))

    def test_reject_stale_flat_hsaco_in_dual_tree(self):
        with tempfile.TemporaryDirectory() as td:
            shutil.copytree(self.real_modules, td, dirs_exist_ok=True)
            stale_path = os.path.join(td, "c32_fast.hsaco")
            with open(stale_path, "wb") as f:
                f.write(b"stale")

            rc, ctx, err = self.call_create(td)
            self.assertEqual(rc, 4)
            self.assertIn("stale flat .hsaco files found in dual-architecture directory", err)

    def test_reject_checksum_mismatch_dual_tree(self):
        with tempfile.TemporaryDirectory() as td:
            shutil.copytree(self.real_modules, td, dirs_exist_ok=True)
            target_hsaco = os.path.join(td, "gfx1200", "c32_fast.hsaco")
            with open(target_hsaco, "r+b") as f:
                b = f.read(1)
                f.seek(0)
                f.write(bytes([b[0] ^ 0xff]))

            rc, ctx, err = self.call_create(td)
            self.assertEqual(rc, 4)
            self.assertIn("checksum mismatch in gfx1200/c32_fast.hsaco", err)

    def test_reject_checksum_mismatch_flat_tree(self):
        with tempfile.TemporaryDirectory() as td:
            leaf_src = os.path.join(self.real_modules, "gfx1201")
            shutil.copytree(leaf_src, td, dirs_exist_ok=True)
            target_hsaco = os.path.join(td, "c32_fast.hsaco")
            with open(target_hsaco, "r+b") as f:
                b = f.read(1)
                f.seek(0)
                f.write(bytes([b[0] ^ 0xff]))

            rc, ctx, err = self.call_create(td)
            self.assertEqual(rc, 4)
            self.assertIn("checksum mismatch in c32_fast.hsaco", err)

    def test_reject_missing_arch_directory(self):
        with tempfile.TemporaryDirectory() as td:
            shutil.copytree(self.real_modules, td, dirs_exist_ok=True)
            shutil.rmtree(os.path.join(td, "gfx1200"))

            rc, ctx, err = self.call_create(td)
            self.assertEqual(rc, 4)
            self.assertIn("dual-architecture directory missing required architecture directory", err)

    def test_reject_missing_module_file(self):
        with tempfile.TemporaryDirectory() as td:
            shutil.copytree(self.real_modules, td, dirs_exist_ok=True)
            os.remove(os.path.join(td, "gfx1201", "wave-pointwise.hsaco"))

            rc, ctx, err = self.call_create(td)
            self.assertEqual(rc, 4)
            self.assertIn("module file missing: gfx1201/wave-pointwise.hsaco", err)

    def test_reject_unsafe_path_traversal(self):
        with tempfile.TemporaryDirectory() as td:
            shutil.copytree(self.real_modules, td, dirs_exist_ok=True)
            sums_path = os.path.join(td, "SHA256SUMS")
            with open(sums_path, "a") as f:
                f.write("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  gfx1200/../evil.hsaco\n")

            rc, ctx, err = self.call_create(td)
            self.assertEqual(rc, 4)
            self.assertIn("unsafe module path", err)

    def test_reject_duplicate_entry(self):
        with tempfile.TemporaryDirectory() as td:
            shutil.copytree(self.real_modules, td, dirs_exist_ok=True)
            sums_path = os.path.join(td, "SHA256SUMS")
            with open(sums_path, "r") as f:
                content = f.read()
            first_line = content.splitlines()[0]
            with open(sums_path, "w") as f:
                f.write(content + "\n" + first_line + "\n")

            rc, ctx, err = self.call_create(td)
            self.assertEqual(rc, 4)
            self.assertIn("duplicate module entry", err)

    def test_reject_incomplete_manifest(self):
        with tempfile.TemporaryDirectory() as td:
            shutil.copytree(self.real_modules, td, dirs_exist_ok=True)
            sums_path = os.path.join(td, "SHA256SUMS")
            with open(sums_path, "r") as f:
                lines = f.readlines()
            with open(sums_path, "w") as f:
                f.writelines(lines[:-1])

            rc, ctx, err = self.call_create(td)
            self.assertEqual(rc, 4)
            self.assertIn("dual-architecture SHA256SUMS incomplete", err)

    def test_reject_leaf_sums_mismatch_with_root(self):
        with tempfile.TemporaryDirectory() as td:
            shutil.copytree(self.real_modules, td, dirs_exist_ok=True)
            leaf_sums = os.path.join(td, "gfx1201", "SHA256SUMS")
            with open(leaf_sums, "r") as f:
                lines = f.readlines()
            bad_hash = "0000000000000000000000000000000000000000000000000000000000000000"
            parts = lines[0].split(None, 1)
            lines[0] = f"{bad_hash}  {parts[1]}"
            with open(leaf_sums, "w") as f:
                f.writelines(lines)

            rc, ctx, err = self.call_create(td)
            self.assertEqual(rc, 4)
            self.assertIn("leaf SHA256SUMS mismatch with root", err)

if __name__ == '__main__':
    unittest.main()
