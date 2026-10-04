import os
import shutil
import tempfile
import unittest
import ctypes
from pathlib import Path
import subprocess
import sys
import textwrap

class LmxxfNrCapabilities(ctypes.Structure):
    _fields_ = [
        ('struct_size', ctypes.c_uint32),
        ('abi_version', ctypes.c_uint32),
        ('max_input_width', ctypes.c_uint32),
        ('max_input_height', ctypes.c_uint32),
        ('history_supported', ctypes.c_uint32),
        ('overlap_supported', ctypes.c_uint32),
        ('graph_supported', ctypes.c_uint32),
        ('gfx1201_target', ctypes.c_uint32),
    ]

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
    ('BeginRecordingExecution', ctypes.c_void_p),
    ('EndRecordingExecution', ctypes.c_void_p),
    ('InvalidateRecording', ctypes.c_void_p),
    ('CollectRecording', ctypes.c_void_p),
    ('GetTimings', ctypes.c_void_p),
]

class RuntimeConfigTests(unittest.TestCase):
    def run_config_probe(self, initial_env, flags, probe, layers=None):
        # Each probe uses a private DLL/flags directory and process so CRT caches,
        # the flags probe timer and environment writes cannot leak between tests.
        runtime = Path(os.environ.get('LMXXF_TEST_RUNTIME', r'exports\lmxxf-runtime\LmxxfNrRuntime.dll')).resolve()
        self.assertTrue(runtime.is_file(), str(runtime))
        with tempfile.TemporaryDirectory(prefix='lmxxf-config-') as td:
            dll = Path(td) / runtime.name
            shutil.copy2(runtime, dll)
            if flags is not None:
                (Path(td) / 'native-game-flags.txt').write_text(flags, encoding='ascii')
            for name, text in (layers or {}).items():
                (Path(td) / name).write_text(text, encoding='utf-8')
            env = {key: value for key, value in os.environ.items() if not key.upper().startswith('DLSS5_')}
            env.update(initial_env)
            source = textwrap.dedent('''
                import ctypes
                import sys
                from tests.lmxxf.test_runtime_validation import LmxxfNrApi, LmxxfNrCapabilities
                dll = ctypes.WinDLL(sys.argv[1])
                api = LmxxfNrApi()
                api.struct_size = ctypes.sizeof(api)
                dll.LmxxfNrGetApi.argtypes = [ctypes.c_uint32, ctypes.POINTER(LmxxfNrApi)]
                assert dll.LmxxfNrGetApi(2, ctypes.byref(api)) == 0
                kernel = ctypes.WinDLL('kernel32', use_last_error=True)
                kernel.SetEnvironmentVariableW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
                kernel.GetEnvironmentVariableW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32]
                def set_env(key, value):
                    assert kernel.SetEnvironmentVariableW(key, value)
                def get_env(key):
                    value = ctypes.create_unicode_buffer(256)
                    n = kernel.GetEnvironmentVariableW(key, value, len(value))
                    return value.value if n else None
                def query_size():
                    caps = LmxxfNrCapabilities()
                    caps.struct_size = ctypes.sizeof(caps)
                    assert api.QueryCapabilities(ctypes.byref(caps)) == 0
                    return caps.max_input_width, caps.max_input_height
            ''') + textwrap.dedent(probe)
            result = subprocess.run(
                [sys.executable, '-c', source, str(dll)], env=env,
                cwd=Path(__file__).resolve().parents[2], capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_three_layers_preserve_host_and_merge_last_values(self):
        self.run_config_probe({'DLSS5_MULTI_PASS': '1'},
            'DLSS5_FAST_NUMERIC=1\nDLSS5_FAST_NUMERIC=0\nDLSS5_STYLE_FEATURE=\n', """
                query_size()
                assert get_env('DLSS5_MULTI_PASS') == '1'
                assert get_env('DLSS5_FAST_NUMERIC') == '0'
                assert get_env('DLSS5_NETWORK_FREE_RES') == '1'
                assert get_env('DLSS5_STYLE_FEATURE') is None
            """, layers={
                'default-config.txt': '\ufeffDLSS5_MULTI_PASS=3\nDLSS5_FAST_NUMERIC=1\nDLSS5_NETWORK_FREE_RES=0\nDLSS5_STYLE_FEATURE=2\n',
                'custom-config.txt': '# custom\nDLSS5_MULTI_PASS=2\nDLSS5_NETWORK_FREE_RES=1   \n',
            })

    def test_live_fit_large_updates_after_crt_initialization(self):
        self.run_config_probe({'DLSS5_FIT_LARGE': '1'}, None, '''
            assert query_size() == (16384, 16384)
            set_env('DLSS5_FIT_LARGE', '0')
            assert query_size() == (1920, 1080)
            set_env('DLSS5_FIT_LARGE', '1')
            assert query_size() == (16384, 16384)
        ''')

    def test_flags_fill_other_keys_when_host_sets_fit_large(self):
        self.run_config_probe(
            {'DLSS5_FIT_LARGE': '1', 'DLSS5_HIP_WAVE_OWNED': '1'},
            'DLSS5_FIT_LARGE=0\nDLSS5_HIP_WAVE_OWNED=0\nDLSS5_TYPELESS_RGBA16=unorm\n', '''
                assert query_size() == (16384, 16384)
                assert get_env('DLSS5_HIP_WAVE_OWNED') == '1'
                assert get_env('DLSS5_TYPELESS_RGBA16') == 'unorm'
            ''',
        )

    def test_live_host_choice_overrides_loaded_flags(self):
        self.run_config_probe({}, 'DLSS5_FIT_LARGE=1\n', '''
            assert query_size() == (16384, 16384)
            set_env('DLSS5_FIT_LARGE', '0')
            assert query_size() == (1920, 1080)
        ''')

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
        rc = cls.get_api(2, ctypes.byref(cls.api))
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
        self.assertIn("modules_ok=76", st)
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
        self.assertIn("modules_ok=38", st)
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

    def test_package_abi_capabilities_and_pdl_status(self):
        caps = LmxxfNrCapabilities()
        caps.struct_size = ctypes.sizeof(LmxxfNrCapabilities)
        rc = self.api.QueryCapabilities(ctypes.byref(caps))
        self.assertEqual(rc, 0)
        self.assertEqual(caps.abi_version, 2)
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
