"""Exercise the shipped launchers with Windows PowerShell 5.1 in temp folders.

No release is repackaged and no real game/author binary is used. Only the
fixture installer's accepted runtime SHA is changed for its success cases.
The batch pause gets an output marker because its own text is localized.
"""
from pathlib import Path
import hashlib
import os
import re
import shutil
import subprocess
import tempfile
import unittest

from lmxxf_fixtures import damage_modules, locked_file, make_modules, snapshot_files


REPO = Path(__file__).resolve().parents[1]
PS = Path(os.environ.get("SystemRoot", r"C:\Windows")) / (
    "System32/WindowsPowerShell/v1.0/powershell.exe"
)
PAUSE_MARKER = "__LAUNCHER_PAUSE__"
FAKE_RUNTIME = b"Harmless installer test data; not an executable."


def write_ps(path, source):
    path.write_bytes(b"\xef\xbb\xbf" + source.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8"))


@unittest.skipUnless(os.name == "nt" and PS.exists(), "requires Windows PowerShell 5.1")
class InstallerExitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="amd-installer-exit-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.package = self.root / "package with spaces"
        self.game = self.root / "game with spaces"
        self.package.mkdir()
        self.game.mkdir()
        self.env = {key.upper(): value for key, value in os.environ.items()}
        # A parent pwsh may export its own PSModulePath. Test the actual PS5
        # runtime with its default module discovery, like a double-click.
        self.env.pop("PSMODULEPATH", None)
        self.env["PATH"] = str(PS.parent) + os.pathsep + self.env.get("PATH", "")
        shutil.copy2(REPO / "tools/lmxxf-module-package.ps1", self.package)
        source = (REPO / "tools/PACKAGE_RELEASE.ps1").read_text(encoding="utf-8-sig")
        for title_name, script, file_name in (
            ("Setup", "install", "Setup"),
            ("Uninstall", "uninstall", "Uninstall_OptiScaler_NR"),
        ):
            body = re.search(
                r"@'\n(@echo off\nsetlocal\ntitle OptiScaler AMD pre-SR "
                + title_name + r"\n.*?)\n'@ \| Set-Content", source, re.S
            )
            self.assertIsNotNone(body, f"missing production {file_name}.bat template")
            batch = body.group(1)
            self.assertEqual(len(re.findall(r"(?m)^pause$", batch)), 1)
            batch = batch.replace("\npause\n", f"\necho {PAUSE_MARKER}\npause\n")
            (self.package / f"{file_name}.bat").write_bytes(batch.replace("\n", "\r\n").encode("ascii"))
            original = (REPO / f"tools/{script}-amd-presr.ps1").read_text(encoding="utf-8-sig")
            write_ps(self.package / f"{file_name}.ps1", original)

    def launcher_files(self, name):
        if name == "Uninstall":
            return "Uninstall_OptiScaler_NR.bat", "Uninstall_OptiScaler_NR.ps1"
        return f"{name}.bat", f"{name}.ps1"

    def run_process(self, args, stdin=""):
        # A timeout must end Setup.bat's PowerShell too. Killing only cmd.exe
        # leaves that child holding stdout, and the test then waits forever.
        proc = subprocess.Popen(
            args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, cwd=self.package, env=self.env,
        )
        try:
            out, _ = proc.communicate(stdin.encode("ascii"), timeout=30)
        except subprocess.TimeoutExpired:
            subprocess.run(
                ["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            try:
                out, _ = proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                out = b""
            text = out.decode("mbcs", errors="replace")
            raise AssertionError("timed out after 30 seconds\n" + text) from None
        return proc.returncode, out.decode("mbcs", errors="replace")

    def run_batch(self, name="Setup", game=None, proxy="dxgi.dll", stdin=""):
        bat, _ = self.launcher_files(name)
        args = [str(self.package / bat), str(game or self.game)]
        if name == "Setup":
            args.append(proxy)
        # cmd's /c outer quoting is separate from each pathname's quoting.
        command = subprocess.list2cmdline(args)
        code, output = self.run_process('cmd.exe /d /s /c "' + command + '"', stdin)
        self.assertEqual(output.count(PAUSE_MARKER), 1, output)
        self.assertNotIn("Press any key to exit...", output)
        return code, output

    def run_direct(self, name="Setup", game=None, flags=("-NonInteractive",)):
        _, script = self.launcher_files(name)
        args = [str(PS), "-NoProfile", "-ExecutionPolicy", "Bypass", "-STA", "-File",
                str(self.package / script), "-GameDir", str(game or self.game)]
        if name == "Setup":
            args += ["-Proxy", "dxgi.dll"]
        code, output = self.run_process(args + list(flags))
        self.assertNotIn("Press any key to exit...", output)
        return code, output

    def ready_install(self):
        script = self.package / "Setup.ps1"
        source = script.read_text(encoding="utf-8-sig")
        digest = hashlib.sha256(FAKE_RUNTIME).hexdigest().upper()
        source, count = re.subn(r"(\$expectedA030\s*=\s*)'[A-F0-9]{64}'", r"\g<1>'" + digest + "'", source)
        self.assertEqual(count, 1, "success fixture must replace only its own trusted SHA")
        write_ps(script, source)
        (self.package / "OptiScaler.dll").write_bytes(b"fixture proxy")
        (self.package / "version.dll").write_bytes(FAKE_RUNTIME)
        (self.package / "dlssnr_on_amd_weights.bin").write_bytes(bytes(1024 * 1024))

    def broken_author_setup(self):
        (self.package / "OptiScaler.dll").write_bytes(b"fixture proxy")
        (self.package / "dlssnr_on_amd_setup.exe").write_bytes(b"invalid executable")

    def test_noninteractive_ini_overwrite_keeps_install_backend(self):
        self.ready_install()
        (self.package / "OptiScaler.ini").write_text(
            "[DlssNr]\nPackageSentinel=yes\nNrBackend=lmxxf\n", encoding="utf-8"
        )
        (self.game / "OptiScaler.ini").write_text(
            "[DlssNr]\nUserSentinel=keep\nNrBackend=lmxxf\n", encoding="utf-8"
        )
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        text = (self.game / "OptiScaler.ini").read_text(encoding="utf-8-sig")
        self.assertIn("PackageSentinel=yes", text)
        self.assertNotIn("UserSentinel", text)
        self.assertIn("NrBackend = daniel", text)
        self.assertNotIn("NrBackend=lmxxf", text)
        self.assertNotIn("NrBackend = lmxxf", text)

    def test_install_success_pauses_once(self):
        self.ready_install()
        code, output = self.run_batch()
        self.assertEqual(code, 0, output)
        self.assertIn("Install SUCCEEDED.", output)
        self.assertNotIn("Setup failed", output)
        self.assertNotIn("Existing OptiScaler installation detected", output)
        self.assertEqual((self.game / "dlssnr_amd_pass1.dll").read_bytes(), FAKE_RUNTIME)
        self.assertTrue((self.game / "Uninstall_OptiScaler_NR.bat").is_file(), output)
        self.assertTrue((self.game / "Uninstall_OptiScaler_NR.ps1").is_file(), output)

    def test_explicit_install_failure_pauses_once(self):
        code, output = self.run_batch(game=self.root / "missing")
        self.assertEqual(code, 1, output)
        self.assertIn("Install FAILED.", output)
        self.assertIn("Setup failed (exit code 1)", output)

    def test_invalid_author_setup_is_caught_and_pauses_once(self):
        self.broken_author_setup()
        code, output = self.run_batch()
        self.assertEqual(code, 1, output)
        self.assertIn("Unexpected install error:", output)
        self.assertIn("Install FAILED.", output)
        self.assertIn("Setup failed (exit code 1)", output)

    def test_parameter_binding_failure_has_batch_fallback(self):
        code, output = self.run_batch(proxy="unsupported.dll")
        self.assertNotEqual(code, 0, output)
        self.assertIn(f"Setup failed (exit code {code})", output)
        self.assertNotIn("Install SUCCEEDED.", output)

    def test_batch_preserves_nonstandard_exit_code(self):
        write_ps(self.package / "Setup.ps1", "param($GameDir, $Proxy, [switch]$NoPause)\nexit 23\n")
        code, output = self.run_batch()
        self.assertEqual(code, 23, output)
        self.assertIn("Setup failed (exit code 23)", output)

    def test_cancel_keeps_confirmation_and_is_not_success(self):
        self.ready_install()
        (self.game / "dxgi.dll").write_bytes(b"another mod")
        code, output = self.run_batch(stdin="1\n")
        self.assertEqual(code, 0, output)
        self.assertIn("Cancelled.", output)
        self.assertNotIn("Install SUCCEEDED.", output)
        self.assertNotIn("Setup failed", output)
        self.assertNotIn("Existing OptiScaler installation detected", output)
        self.assertEqual((self.game / "dxgi.dll").read_bytes(), b"another mod")

    def test_noninteractive_install_success_does_not_wait(self):
        self.ready_install()
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("Install SUCCEEDED.", output)

    def test_noninteractive_install_failures_do_not_wait(self):
        code, output = self.run_direct(game=self.root / "missing")
        self.assertEqual(code, 1, output)
        self.assertIn("Install FAILED.", output)
        self.broken_author_setup()
        code, output = self.run_direct()
        self.assertEqual(code, 1, output)
        self.assertIn("Unexpected install error:", output)

    def test_uninstall_copied_into_game_folder_runs_in_place(self):
        """Setup copies Uninstall_OptiScaler_NR into the game folder.

        Double-click there has no folder picker: the script uses its own
        directory. Cancel must keep files; Y deletes the planned list.
        """
        self.ready_install()
        code, output = self.run_batch()
        self.assertEqual(code, 0, output)
        bat = self.game / "Uninstall_OptiScaler_NR.bat"
        script = self.game / "Uninstall_OptiScaler_NR.ps1"
        self.assertTrue(bat.is_file(), output)
        self.assertTrue(script.is_file(), output)
        pass1 = self.game / "dlssnr_amd_pass1.dll"
        self.assertTrue(pass1.is_file(), output)

        def run_in_place(stdin):
            args = [str(PS), "-NoProfile", "-ExecutionPolicy", "Bypass", "-STA",
                    "-File", str(script), "-NoPause"]
            return self.run_process(args, stdin)

        # Setup always creates backup-amd-presr-*, so keep-backups is asked first.
        code, output = run_in_place("Y\nN\n")
        self.assertEqual(code, 0, output)
        self.assertIn("Planned deletions", output)
        self.assertIn("Cancelled.", output)
        self.assertNotIn("Uninstall SUCCEEDED.", output)
        self.assertTrue(pass1.is_file(), output)
        self.assertTrue(script.is_file(), output)

        code, output = run_in_place("Y\nY\n")
        self.assertEqual(code, 0, output)
        self.assertIn("Planned deletions", output)
        self.assertIn("Uninstall SUCCEEDED.", output)
        self.assertFalse(pass1.exists(), output)
        self.assertFalse(script.exists(), output)
        self.assertFalse(bat.exists(), output)

    def test_uninstall_asks_keep_backups_before_planned_list(self):
        self.ready_install()
        code, output = self.run_batch()
        self.assertEqual(code, 0, output)
        script = self.game / "Uninstall_OptiScaler_NR.ps1"
        backup = self.game / "backup-amd-presr-fixture"
        backup.mkdir()
        (backup / "old.dll").write_bytes(b"old")
        pass1 = self.game / "dlssnr_amd_pass1.dll"

        def run_in_place(stdin):
            args = [str(PS), "-NoProfile", "-ExecutionPolicy", "Bypass", "-STA",
                    "-File", str(script), "-NoPause"]
            return self.run_process(args, stdin)

        code, output = run_in_place("Y\nN\n")
        self.assertEqual(code, 0, output)
        keep_at = output.lower().find("old backup folder")
        planned_at = output.lower().find("planned deletions")
        self.assertNotEqual(keep_at, -1, output)
        self.assertNotEqual(planned_at, -1, output)
        self.assertLess(keep_at, planned_at, output)
        self.assertIn("Cancelled.", output)
        self.assertTrue(backup.is_dir(), output)
        self.assertTrue(pass1.is_file(), output)

        code, output = run_in_place("n\ny\n")
        self.assertEqual(code, 0, output)
        self.assertIn("Uninstall SUCCEEDED.", output)
        self.assertFalse(backup.exists(), output)
        self.assertFalse(pass1.exists(), output)

    def test_uninstall_success_and_cancel_pause_once(self):
        for answer, message in (
            ("Y\n", "Uninstall SUCCEEDED."),
            ("yes\n", "Uninstall SUCCEEDED."),
            ("N\n", "Cancelled."),
            ("NO\n", "Cancelled."),
        ):
            with self.subTest(answer=answer.strip()):
                code, output = self.run_batch(name="Uninstall", stdin=answer)
                self.assertEqual(code, 0, output)
                self.assertIn(message, output)
                self.assertNotIn("Uninstall failed", output)
                if answer.startswith("N"):
                    self.assertNotIn("Uninstall SUCCEEDED.", output)

    def test_uninstall_explicit_failure_pauses_once(self):
        code, output = self.run_batch(name="Uninstall", game=self.root / "missing")
        self.assertEqual(code, 1, output)
        self.assertIn("Uninstall FAILED.", output)
        self.assertIn("Uninstall failed (exit code 1)", output)

    def test_uninstall_unhandled_exception_uses_trap(self):
        _, script_name = self.launcher_files("Uninstall")
        script = self.package / script_name
        source = script.read_text(encoding="utf-8-sig")
        insertion = "$game = (Resolve-Path -LiteralPath $GameDir).Path"
        self.assertEqual(source.count(insertion), 1)
        write_ps(script, source.replace(insertion, insertion + "\nthrow 'fixture unexpected error'"))
        code, output = self.run_batch(name="Uninstall")
        self.assertEqual(code, 1, output)
        self.assertIn("fixture unexpected error", output)
        self.assertIn("Uninstall FAILED.", output)

    def test_uninstall_parse_failure_has_batch_fallback(self):
        _, script_name = self.launcher_files("Uninstall")
        write_ps(self.package / script_name, "param(\n")
        code, output = self.run_batch(name="Uninstall")
        self.assertNotEqual(code, 0, output)
        self.assertIn(f"Uninstall failed (exit code {code})", output)

    def test_noninteractive_uninstall_does_not_wait(self):
        for game, expected in ((self.game, 0), (self.root / "missing", 1)):
            with self.subTest(expected=expected):
                code, output = self.run_direct(name="Uninstall", game=game)
                self.assertEqual(code, expected, output)


    def ready_lmxxf_dual_arch(self):
        (self.package / "OptiScaler.dll").write_bytes(b"fixture proxy")
        (self.package / "LmxxfNrRuntime.dll").write_bytes(b"fixture lmxxf runtime")
        make_modules(self.package / "lmxxf-modules")

        shaders = self.package / "shaders"
        shaders.mkdir(parents=True, exist_ok=True)
        (shaders / "native_codec_encode.hlsl").write_text("// shader fixture", encoding="utf-8")

    def test_dual_arch_lmxxf_clean_install(self):
        self.ready_lmxxf_dual_arch()
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("Install SUCCEEDED.", output)
        self.assertTrue((self.game / "LmxxfNrRuntime.dll").is_file(), output)
        game_mods = self.game / "lmxxf-modules"
        self.assertTrue((game_mods / "gfx1200").is_dir(), output)
        self.assertTrue((game_mods / "gfx1201").is_dir(), output)
        self.assertEqual(len(list(game_mods.glob("*.hsaco"))), 0, "no flat hsaco in root")
        self.assertEqual(len(list((game_mods / "gfx1200").glob("*.hsaco"))), 24)
        self.assertEqual(len(list((game_mods / "gfx1201").glob("*.hsaco"))), 24)
        self.assertTrue((game_mods / "SHA256SUMS").is_file(), output)
        self.assertTrue((game_mods / "runtime-manifest.json").is_file(), output)

    def ready_legacy_install(self):
        self.ready_lmxxf_dual_arch()
        mods = self.game / "lmxxf-modules"
        shutil.copytree(self.package / "lmxxf-modules/gfx1201", mods)
        (self.game / "dxgi.dll").write_bytes(b"fixture proxy")
        (self.game / "LmxxfNrRuntime.dll").write_bytes(b"old runtime")
        (self.game / "OptiScaler.ini").write_text("[DlssNr]\nUserSentinel=keep\n", encoding="utf-8")
        (mods / "user-custom.hsaco").write_bytes(b"user-owned GPU code")
        (mods / "user_weights.bin").write_bytes(b"user weights")
        return mods

    def assert_dual_arch_installed(self):
        mods = self.game / "lmxxf-modules"
        self.assertFalse(list(mods.glob("*.hsaco")))
        self.assertFalse((mods / "modules.json").exists())
        for arch in ("gfx1200", "gfx1201"):
            self.assertEqual(len(list((mods / arch).glob("*.hsaco"))), 24)
        return mods

    def assert_custom_module_backed_up(self):
        saved = list(self.game.glob("backup-amd-presr-*/lmxxf-modules/user-custom.hsaco"))
        self.assertEqual(len(saved), 1)
        self.assertEqual(saved[0].read_bytes(), b"user-owned GPU code")

    def test_legacy_upgrade_noninteractive_overwrites_with_backup(self):
        mods = self.ready_legacy_install()
        before = snapshot_files(mods)
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("Install SUCCEEDED", output)
        self.assertNotIn("Uninstalling the existing", output)
        self.assert_dual_arch_installed()
        self.assert_custom_module_backed_up()
        backups = list(self.game.glob("backup-amd-presr-*/lmxxf-modules"))
        self.assertEqual(snapshot_files(backups[0]), before)
        self.assertEqual((mods / "user_weights.bin").read_bytes(), b"user weights")

    def test_setup_no_overwrites_legacy_without_uninstall_or_second_proxy_prompt(self):
        self.ready_legacy_install()
        code, output = self.run_batch(stdin="n\n")
        self.assertEqual(code, 0, output)
        self.assertEqual(output.count("Existing OptiScaler installation detected"), 1, output)
        self.assertIn("Recommended: uninstall before installing this version", output)
        self.assertIn("Continuing with an overwrite installation", output)
        self.assertNotIn("Uninstalling the existing", output)
        self.assertNotIn("How to continue?", output)
        self.assert_dual_arch_installed()
        self.assert_custom_module_backed_up()
        self.assertIn("UserSentinel", (self.game / "OptiScaler.ini").read_text(encoding="utf-8-sig"))

    def test_setup_yes_uses_new_uninstaller_then_installs_without_second_confirmation(self):
        self.ready_legacy_install()
        (self.package / "OptiScaler.ini").write_text("[DlssNr]\nPackageSentinel=yes\n", encoding="utf-8")
        (self.game / "dlssnr_amd_pass3.dll").write_bytes(b"old unused backend")
        write_ps(self.game / "Uninstall_OptiScaler_NR.ps1", "throw 'old uninstaller must not run'\n")
        backup = self.game / "backup-amd-presr-existing"
        backup.mkdir()
        (backup / "keep.bin").write_bytes(b"old backup")
        code, output = self.run_batch(stdin="Y\n")
        self.assertEqual(code, 0, output)
        self.assertIn("Uninstall SUCCEEDED.", output)
        self.assertIn("Install SUCCEEDED.", output)
        self.assertLess(output.index("Uninstall SUCCEEDED."), output.index("Installing OptiScaler as"))
        self.assertNotIn("Type Y to delete", output)
        self.assertNotIn("Keep these backup folders?", output)
        self.assertNotIn("old uninstaller must not run", output)
        self.assertFalse((self.game / "dlssnr_amd_pass3.dll").exists())
        self.assertEqual((backup / "keep.bin").read_bytes(), b"old backup")
        self.assert_dual_arch_installed()
        self.assert_custom_module_backed_up()
        ini = (self.game / "OptiScaler.ini").read_text(encoding="utf-8-sig")
        self.assertIn("PackageSentinel", ini)
        self.assertNotIn("UserSentinel", ini)

    def test_existing_install_prompt_precedes_proxy_menu(self):
        self.ready_legacy_install()
        args = [str(PS), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                str(self.package / "Setup.ps1"), "-GameDir", str(self.game), "-NoPause"]
        code, output = self.run_process(args, "NO\n1\n")
        self.assertEqual(code, 0, output)
        self.assertLess(output.index("Existing OptiScaler installation detected"), output.index("Which proxy DLL"))

    def test_setup_overwrite_moves_other_opti_proxy_without_reprompt(self):
        self.ready_legacy_install()
        (self.game / "dxgi.dll").rename(self.game / "winmm.dll")
        code, output = self.run_batch(stdin="N\n")
        self.assertEqual(code, 0, output)
        self.assertFalse((self.game / "winmm.dll").exists())
        self.assertEqual((self.game / "dxgi.dll").read_bytes(), b"fixture proxy")
        self.assertTrue(list(self.game.glob("backup-amd-presr-*/winmm.dll.moved")))
        self.assertNotIn("How to continue?", output)

    def test_setup_uninstall_failure_stops_before_install(self):
        self.ready_legacy_install()
        write_ps(self.package / "Uninstall_OptiScaler_NR.ps1",
                 "param($GameDir, [switch]$NonInteractive, [switch]$NoPause)\nexit 7\n")
        before = snapshot_files(self.game)
        code, output = self.run_batch(stdin="Y\n")
        self.assertEqual(code, 1, output)
        self.assertIn("Uninstall failed (exit code 7)", output)
        self.assertNotIn("Install SUCCEEDED", output)
        self.assertEqual(snapshot_files(self.game), before)
        self.assertFalse(list(self.game.glob(".lmxxf-stage-*")))

    def test_setup_missing_new_uninstaller_preserves_old_install(self):
        self.ready_legacy_install()
        (self.package / "Uninstall_OptiScaler_NR.ps1").unlink()
        before = snapshot_files(self.game)
        code, output = self.run_batch(stdin="Y\n")
        self.assertEqual(code, 1, output)
        self.assertIn("Missing Uninstall_OptiScaler_NR.ps1", output)
        self.assertEqual(snapshot_files(self.game), before)

    def test_setup_corrupt_package_is_rejected_before_agreed_uninstall(self):
        self.ready_legacy_install()
        before = snapshot_files(self.game)
        damage_modules(self.package / "lmxxf-modules", "corrupt")
        code, output = self.run_batch(stdin="Y\n")
        self.assertEqual(code, 1, output)
        self.assertIn("checksum mismatch", output)
        self.assertNotIn("Uninstalling the existing", output)
        self.assertEqual(snapshot_files(self.game), before)

    def test_noninteractive_uninstall_requires_explicit_opt_in(self):
        self.ready_legacy_install()
        code, output = self.run_direct(flags=("-NonInteractive", "-UninstallExisting"))
        self.assertEqual(code, 0, output)
        self.assertIn("Uninstall SUCCEEDED.", output)
        self.assertIn("Install SUCCEEDED.", output)

    def test_setup_clean_reinstall_preserves_reused_game_runtime_and_shaders(self):
        self.ready_lmxxf_dual_arch()
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        (self.package / "LmxxfNrRuntime.dll").unlink()
        (self.package / "shaders/native_codec_encode.hlsl").unlink()
        code, output = self.run_batch(stdin="yes\n")
        self.assertEqual(code, 0, output)
        self.assertIn("Uninstall SUCCEEDED.", output)
        self.assertEqual((self.game / "LmxxfNrRuntime.dll").read_bytes(), b"fixture lmxxf runtime")
        self.assertTrue((self.game / "shaders/native_codec_encode.hlsl").is_file())
        self.assertFalse(list(self.game.glob(".amd-presr-source-*")))

    def test_setup_clean_reinstall_from_package_in_game_preserves_its_sources(self):
        self.ready_lmxxf_dual_arch()
        (self.package / "dxgi.dll").write_bytes(b"fixture proxy")
        code, output = self.run_batch(game=self.package, stdin="Y\n")
        self.assertEqual(code, 0, output)
        self.assertIn("Uninstall SUCCEEDED.", output)
        self.assertTrue((self.package / "LmxxfNrRuntime.dll").is_file())
        self.assertTrue((self.package / "Uninstall_OptiScaler_NR.ps1").is_file())
        self.assertTrue((self.package / "lmxxf-module-package.ps1").is_file())
        self.assertFalse(list(self.package.glob(".amd-presr-source-*")))

    def test_locked_legacy_module_is_preserved(self):
        mods = self.ready_legacy_install()
        before = snapshot_files(self.game)
        # The old directory cannot move: no partial publication or file cleanup.
        with locked_file(mods, share=3, directory=True):
            code, output = self.run_direct()
        self.assertEqual(code, 1, output)
        self.assertIn("Could not install lmxxf-modules", output)
        self.assertEqual(snapshot_files(self.game), before)

    def test_uninstall_then_install_preserves_extra_hsaco(self):
        mods = self.ready_legacy_install()
        code, output = self.run_direct(name="Uninstall")
        self.assertEqual(code, 0, output)
        self.assertEqual((mods / "user-custom.hsaco").read_bytes(), b"user-owned GPU code")
        self.assertEqual(list(mods.glob("*.hsaco")), [mods / "user-custom.hsaco"])
        self.assertTrue((mods / "user_weights.bin").exists())
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assert_custom_module_backed_up()
        self.assert_dual_arch_installed()
        self.assertTrue((mods / "user_weights.bin").exists())

    def test_dual_arch_reinstall_and_uninstall_preserve_user_files(self):
        self.ready_lmxxf_dual_arch()
        mods = self.game / "lmxxf-modules"
        make_modules(mods, marker="old")
        (mods / "user_extra").mkdir()
        (mods / "user_extra/info.txt").write_bytes(b"keep me")
        (mods / "user_weights.bin").write_bytes(b"weights")
        for _ in range(2):
            code, output = self.run_direct()
            self.assertEqual(code, 0, output)
            self.assertEqual((mods / "user_extra/info.txt").read_bytes(), b"keep me")
            self.assertEqual((mods / "user_weights.bin").read_bytes(), b"weights")
        # Extra modules added by the user after installation are not uninstaller-owned.
        (mods / "gfx1200/user-custom.hsaco").write_bytes(b"extra")
        code, output = self.run_direct(name="Uninstall")
        self.assertEqual(code, 0, output)
        self.assertEqual((mods / "gfx1200/user-custom.hsaco").read_bytes(), b"extra")
        self.assertFalse((mods / "gfx1200/c32_fast.hsaco").exists())
        self.assertFalse((mods / "gfx1201").exists())
        self.assertTrue((mods / "user_extra/info.txt").exists())

    def assert_invalid_package_preserves_install(self, kind):
        self.ready_lmxxf_dual_arch()
        make_modules(self.game / "lmxxf-modules", marker="old")
        (self.game / "LmxxfNrRuntime.dll").write_bytes(b"old runtime")
        (self.game / "OptiScaler.ini").write_bytes(b"old configuration")
        # An invalid module input must fail even before a selected external setup runs.
        (self.package / "dlssnr_on_amd_setup.exe").write_bytes(b"must never be launched")
        before = snapshot_files(self.game)
        damage_modules(self.package / "lmxxf-modules", kind)
        code, output = self.run_direct()
        self.assertEqual(code, 1, output)
        self.assertNotIn("Install SUCCEEDED", output)
        self.assertNotIn("Launching danielblnc setup", output)
        self.assertEqual(snapshot_files(self.game), before)
        self.assertFalse(list(self.game.glob(".lmxxf-stage-*")))
        self.assertFalse(list(self.game.glob("backup-amd-presr-*")))
        return output

    def test_corrupt_module_rejected_before_dll_or_ini_changes(self):
        self.assertIn("checksum mismatch", self.assert_invalid_package_preserves_install("corrupt"))

    def test_missing_arch_rejected_before_install(self):
        self.assert_invalid_package_preserves_install("missing-arch")

    def test_missing_root_manifest_cannot_fall_back_to_installed_modules(self):
        self.assert_invalid_package_preserves_install("missing-root")

    def test_missing_leaf_rejected_before_install(self):
        self.assert_invalid_package_preserves_install("missing-leaf")

    def test_parent_leaf_mismatch_rejected_before_install(self):
        self.assert_invalid_package_preserves_install("leaf-mismatch")

    def test_same_count_renamed_module_rejected_before_install(self):
        self.assert_invalid_package_preserves_install("rename")

    def test_inconsistent_metadata_rejected_before_install(self):
        self.assert_invalid_package_preserves_install("wrong-metadata")

    def test_locked_source_leaves_old_modules_and_runtime_intact(self):
        self.ready_lmxxf_dual_arch()
        make_modules(self.game / "lmxxf-modules", marker="old")
        (self.game / "LmxxfNrRuntime.dll").write_bytes(b"old runtime")
        before = snapshot_files(self.game)
        with locked_file(self.package / "lmxxf-modules/gfx1201/wave-pointwise.hsaco"):
            code, output = self.run_direct()
        self.assertEqual(code, 1, output)
        self.assertEqual(snapshot_files(self.game), before)

    def test_failed_module_directory_switch_leaves_old_install_intact(self):
        self.ready_lmxxf_dual_arch()
        mods = make_modules(self.game / "lmxxf-modules", marker="old")
        (self.game / "LmxxfNrRuntime.dll").write_bytes(b"old runtime")
        before = snapshot_files(self.game)
        with locked_file(mods, share=3, directory=True):
            code, output = self.run_direct()
        self.assertEqual(code, 1, output)
        self.assertIn("Could not install lmxxf-modules", output)
        self.assertEqual(snapshot_files(self.game), before)
        self.assertFalse(list(self.game.glob(".lmxxf-stage-*")))


if __name__ == "__main__":
    unittest.main(verbosity=2)
