import os
import shutil
import tempfile
import unittest
import ctypes

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
    ('QueryCapabilities', ctypes.c_void_p),
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
        dll_path = os.path.abspath(r'exports\lmxxf-runtime\LmxxfNrRuntime.dll')
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

        cls.real_modules = os.path.abspath(r'third_party\lmxxf\modules')

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
        self.assertIn("hip=0", st)

        self.assertEqual(self.api.Destroy(ctx), 0)

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
