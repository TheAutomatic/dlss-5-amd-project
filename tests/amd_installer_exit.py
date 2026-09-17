"""Exercise the shipped launchers with Windows PowerShell 5.1 in temp folders.

No release is repackaged and no real game/author binary is used. Only the
fixture installer's accepted runtime SHA is changed for its success cases.
The batch pause gets an output marker because its own text is localized.
"""
from pathlib import Path
import hashlib
import os
import re
import subprocess
import tempfile
import unittest


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
        result = subprocess.run(
            args, input=stdin.encode("ascii"), stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, cwd=self.package, env=self.env, timeout=30,
        )
        return result.returncode, result.stdout.decode("mbcs", errors="replace")

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

    def test_install_success_pauses_once(self):
        self.ready_install()
        code, output = self.run_batch()
        self.assertEqual(code, 0, output)
        self.assertIn("Install SUCCEEDED.", output)
        self.assertNotIn("Setup failed", output)
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
