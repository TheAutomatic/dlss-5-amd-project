"""Exercise the shipped launchers with Windows PowerShell 5.1 in temp folders.

No release is repackaged and no real game/author binary is used. Only the
fixture installer's accepted runtime SHA is changed for its success cases.
The batch pause gets an output marker because its own text is localized.
"""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "_lib"))
from lmxxf_fixtures import damage_modules, locked_file, make_modules, make_weights, snapshot_files
from mochizuki_fixtures import int4_tools, PAYLOAD as INT4_PAYLOAD


REPO = Path(__file__).resolve().parents[2]
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
        # Actions TEMP may use RUNNER~1; Setup resolves it to the long account name.
        # Compare diagnostics against the same canonical path, including spaces/Unicode.
        self.root = Path(self.temp.name).resolve()
        self.package = self.root / "OptScaler(NR) package with spaces"
        self.game = self.root / "game with spaces"
        self.package.mkdir()
        self.game.mkdir()
        shutil.copy2(REPO / "VERSION", self.package)
        self.env = {key.upper(): value for key, value in os.environ.items()}
        # A parent pwsh may export its own PSModulePath. Test the actual PS5
        # runtime with its default module discovery, like a double-click.
        self.env.pop("PSMODULEPATH", None)
        self.env["PATH"] = str(PS.parent) + os.pathsep + self.env.get("PATH", "")
        shutil.copy2(REPO / "tools/lmxxf-module-package.ps1", self.package)
        shutil.copy2(REPO / "tools/install/mochizuki-python.ps1", self.package)
        for name in ('lmxxf-small.ps1', 'lmxxf-small-manifest.json'):
            shutil.copy2(REPO / 'tools/install' / name, self.package)
        source = (REPO / "tools/release/PACKAGE_RELEASE.ps1").read_text(encoding="utf-8-sig")
        for title_name, script, file_name in (
            ("Setup", "install", "Setup"),
            ("Uninstall", "uninstall", "Uninstall_OptiScaler_NR"),
        ):
            body = re.search(
                r"@'\n(@echo off\nsetlocal\ntitle OptScaler\(NR\) @VERSION@ - "
                + title_name + r"\n.*?)\n'@", source, re.S
            )
            self.assertIsNotNone(body, f"missing production {file_name}.bat template")
            batch = body.group(1).replace("@VERSION@", (REPO / "VERSION").read_text().strip())
            self.assertEqual(len(re.findall(r"(?m)^pause$", batch)), 1)
            batch = batch.replace("\npause\n", f"\necho {PAUSE_MARKER}\npause\n")
            (self.package / f"{file_name}.bat").write_bytes(batch.replace("\n", "\r\n").encode("ascii"))
            original = (REPO / f"tools/{script}-amd-presr.ps1").read_text(encoding="utf-8-sig")
            if script == "install":
                orig_hash = hashlib.sha256(b"NRMODEL1" + (599).to_bytes(4, "little") + bytes(20)).hexdigest()
                small_hash = hashlib.sha256(b"NRMODEL1" + (255).to_bytes(4, "little") + bytes(20)).hexdigest()
                original = re.sub(r"(\$knownOriginalSha\s*=\s*)'[a-f0-9]{64}'", r"\g<1>'" + orig_hash + "'", original)
                original = re.sub(r"(\$knownSmallSha\s*=\s*)'[a-f0-9]{64}'", r"\g<1>'" + small_hash + "'", original)
            write_ps(self.package / f"{file_name}.ps1", original)

    def launcher_files(self, name):
        if name == "Uninstall":
            return "Uninstall_OptiScaler_NR.bat", "Uninstall_OptiScaler_NR.ps1"
        return f"{name}.bat", f"{name}.ps1"

    def assert_uninstaller_path(self, output, expected):
        paths = re.findall(r"(?m)^Uninstall script: (.+)$", output)
        self.assertEqual(len(paths), 1, output)
        # Actions may supply TEMP through RUNNER~1 while PowerShell expands it
        # to runneradmin. Compare file identity, still rejecting the old copy.
        self.assertTrue(Path(paths[0].strip()).samefile(expected), output)

    def run_process(self, args, stdin="", timeout=30):
        # A timeout must end Setup.bat's PowerShell too. Killing only cmd.exe
        # leaves that child holding stdout, and the test then waits forever.
        proc = subprocess.Popen(
            args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, cwd=self.package, env=self.env,
        )
        try:
            out, _ = proc.communicate(stdin.encode("ascii"), timeout=timeout)
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
            raise AssertionError(f"timed out after {timeout} seconds\n" + text) from None
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

    def backend_decisions(self, backends):
        path = self.root / "backend-decisions.json"
        path.write_text(json.dumps({"backends": backends, "existingIni": "keep"}), encoding="utf-8")
        return ("-NonInteractive", "-DecisionJson", str(path))

    def test_explicit_active_backend_survives_install_update_and_reinstall(self):
        self.ready_install()
        self.ready_lmxxf_dual_arch()
        (self.package / "OptiScaler.ini").write_text(
            "[DlssNr]\nNrBackend=lmxxf\nAmdModelScale=0.75\n", encoding="utf-8"
        )
        flags = self.backend_decisions(["lmxxf", "daniel"]) + ("-ActiveBackend", "daniel")
        for action in ("install", "overwrite", "reinstall"):
            with self.subTest(action=action):
                if action == "reinstall":
                    code, output = self.run_direct(name="Uninstall")
                    self.assertEqual(code, 0, output)
                code, output = self.run_direct(flags=flags)
                self.assertEqual(code, 0, output)
                self.assertIn("Active Backend: daniel", output)
                ini = (self.game / "OptiScaler.ini").read_text(encoding="utf-8-sig")
                self.assertRegex(ini, r"NrBackend\s*=\s*daniel")
                self.assertRegex(ini, r"AmdModelScale\s*=\s*0\.75")
                record = (self.game / "amd-presr-install.txt").read_text(encoding="utf-8-sig")
                self.assertIn("backend=daniel", record)

    def test_explicit_backend_list_without_active_uses_first_available(self):
        self.ready_install()
        self.ready_lmxxf_dual_arch()
        code, output = self.run_direct(flags=self.backend_decisions(["daniel", "lmxxf"]))
        self.assertEqual(code, 0, output)
        self.assertIn("Active Backend: lmxxf", output)

    def test_explicit_mochizuki_active_backend_with_multiple_backends(self):
        self.ready_install()
        self.ready_mochizuki(model=True)
        (self.package / "OptiScaler.ini").write_text("[DlssNr]\nNrBackend=lmxxf\n", encoding="utf-8")
        flags = self.backend_decisions(["daniel", "mochizuki"]) + (
            "-ActiveBackend", "mochizuki", "-MochizukiModels", "original"
        )
        code, output = self.run_direct(flags=flags)
        self.assertEqual(code, 0, output)
        self.assertIn("Active Backend: mochizuki", output)
        self.assertRegex((self.game / "OptiScaler.ini").read_text(encoding="utf-8-sig"),
                         r"NrBackend\s*=\s*mochizuki")

    def test_failed_active_backend_falls_back_to_installed_backend(self):
        self.ready_install()
        self.ready_lmxxf_dual_arch()
        (self.package / "dlssnr_on_amd_weights.bin").write_bytes(b"truncated")
        code, output = self.run_direct(
            flags=self.backend_decisions(["lmxxf", "daniel"]) + ("-ActiveBackend", "daniel")
        )
        self.assertEqual(code, 0, output)
        self.assertIn("Skipping daniel: weights check failed", output)
        self.assertIn("Active backend changed to lmxxf", output)
        self.assertIn("Active Backend: lmxxf", output)
        self.assertFalse((self.game / "dlssnr_amd_pass1.dll").exists())

    def test_invalid_or_unselected_active_backend_does_not_change_game(self):
        self.ready_install()
        self.ready_lmxxf_dual_arch()
        before = snapshot_files(self.game)
        for active in ("unknown", "", "mochizuki"):
            with self.subTest(active=active):
                code, output = self.run_direct(
                    flags=self.backend_decisions(["lmxxf", "daniel"]) + ("-ActiveBackend", active)
                )
                self.assertEqual(code, 1, output)
                self.assertIn("ActiveBackend", output)
                self.assertEqual(snapshot_files(self.game), before)

    def test_active_backend_requires_explicit_backend_list(self):
        self.ready_install()
        code, output = self.run_direct(flags=("-NonInteractive", "-ActiveBackend", "daniel"))
        self.assertEqual(code, 1, output)
        self.assertIn("ActiveBackend requires", output)
        self.assertFalse((self.game / "dxgi.dll").exists())

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

    def test_overwrite_and_clean_reinstall_remove_legacy_core_and_enabler_logs(self):
        self.ready_install()
        removed = ('dlssnr_core.dll', 'dlss-enabler.log', 'dlss-enabler.1.log', 'dlss-enabler.log.2')
        kept = ('dlssnr_core_custom.dll', 'dlss-enabler-headless.dll', 'dlss-enabler.ini',
                'dlss-enabler.notes.log', 'dlss-enabler.log.bak')
        for clean in (False, True):
            with self.subTest(clean=clean):
                for folder in (self.game, self.game / '_storage_'):
                    folder.mkdir(exist_ok=True)
                    for name in removed + kept:
                        (folder / name).write_bytes(b'legacy fixture')
                flags = ('-NonInteractive', '-UninstallExisting') if clean else ('-NonInteractive',)
                code, output = self.run_direct(flags=flags)
                self.assertEqual(code, 0, output)
                for folder in (self.game, self.game / '_storage_'):
                    for name in removed:
                        self.assertFalse((folder / name).exists(), name)
                    for name in kept:
                        self.assertEqual((folder / name).read_bytes(), b'legacy fixture', name)
                self.assertFalse(list(self.game.glob('backup-amd-presr-*')))
                self.assertEqual((self.game / 'dlssnr_amd_pass1.dll').read_bytes(), FAKE_RUNTIME)
                self.assertTrue((self.game / 'dlssnr_on_amd_weights.bin').is_file())

    def test_locked_legacy_artifacts_stop_before_overwrite(self):
        self.ready_install()
        for relative in ('dlssnr_core.dll', '_storage_/dlss-enabler.log'):
            with self.subTest(relative=relative):
                target = self.game / relative
                target.parent.mkdir(exist_ok=True)
                target.write_bytes(b'locked legacy artifact')
                before = snapshot_files(self.game)
                with locked_file(target):
                    code, output = self.run_direct()
                self.assertEqual(code, 1, output)
                self.assertIn('file lock detected', output)
                self.assertEqual(snapshot_files(self.game), before)

    def test_kept_ini_migrates_legacy_preferences(self):
        self.ready_install()
        ini = self.game / "OptiScaler.ini"
        ini.write_text(
            "[Other]\nLmxxfFitLarge=true\nLmxxfPdl=true\n"
            "[DlssNr]\nLmxxfFitLarge=false\nLmxxfPdl=false\n", encoding="utf-8"
        )
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        text = ini.read_text(encoding="utf-8-sig")
        self.assertIn("DLSS5_FIT_LARGE = false", text)
        self.assertIn("DLSS5_HIP_PDL = false", text)

    def test_kept_ini_preserves_canonical_preferences_over_legacy(self):
        self.ready_install()
        ini = self.game / "OptiScaler.ini"
        ini.write_text(
            "[DlssNr]\nLmxxfFitLarge=true\nLmxxfPdl=false\n"
            "DLSS5_FIT_LARGE=false\nDLSS5_HIP_PDL=true\n", encoding="utf-8"
        )
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        text = ini.read_text(encoding="utf-8-sig")
        self.assertIn("DLSS5_FIT_LARGE=false", text)
        self.assertIn("DLSS5_HIP_PDL=true", text)
        self.assertEqual(text.count("DLSS5_FIT_LARGE"), 1)
        self.assertEqual(text.count("DLSS5_HIP_PDL"), 1)

    def test_kept_ini_defaults_only_missing_preferences(self):
        self.ready_install()
        ini = self.game / "OptiScaler.ini"
        ini.write_text("[DlssNr]\nLmxxfFitLarge=false\n", encoding="utf-8")
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        text = ini.read_text(encoding="utf-8-sig")
        self.assertIn("DLSS5_FIT_LARGE = false", text)
        self.assertIn("DLSS5_HIP_PDL = true", text)

    def test_interactive_keep_ini_migrates_legacy_preferences(self):
        self.ready_legacy_install()
        (self.package / "OptiScaler.ini").write_text(
            "[DlssNr]\nDLSS5_FIT_LARGE=true\nDLSS5_HIP_PDL=true\n", encoding="utf-8"
        )
        ini = self.game / "OptiScaler.ini"
        ini.write_text("[DlssNr]\nLmxxfFitLarge=false\nLmxxfPdl=false\n", encoding="utf-8")
        code, output = self.run_batch(stdin="n\n3\n2\n")
        self.assertEqual(code, 0, output)
        text = ini.read_text(encoding="utf-8-sig")
        self.assertIn("DLSS5_FIT_LARGE = false", text)
        self.assertIn("DLSS5_HIP_PDL = false", text)

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

        # A clean install no longer creates an empty backup directory.
        code, output = run_in_place("N\n")
        self.assertEqual(code, 0, output)
        self.assertIn("Planned deletions", output)
        self.assertIn("Cancelled.", output)
        self.assertNotIn("Uninstall SUCCEEDED.", output)
        self.assertTrue(pass1.is_file(), output)
        self.assertTrue(script.is_file(), output)

        code, output = run_in_place("Y\n")
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
        weights = self.package / "native-game-tiled-assets"
        make_weights(weights)

        shaders = self.package / "shaders"
        shaders.mkdir(parents=True, exist_ok=True)
        (shaders / "native_codec_encode.hlsl").write_text("// shader fixture", encoding="utf-8")

    def ready_mochizuki(self, model=False):
        (self.package / "OptiScaler.dll").write_bytes(b"fixture proxy")
        (self.package / "MochizukiNrRuntime.dll").write_bytes(b"fixture runtime")
        shaders = self.package / "dlssnr-amd/shaders"
        shaders.mkdir(parents=True)
        (shaders / "fixture.spv").write_bytes(b"fixture shader")
        aco = self.package / "dlssnr-amd/aco"
        (aco / "records").mkdir(parents=True)
        (aco / "shaders").mkdir()
        (aco / "records/0123456789abcdef.nrp").write_bytes(b"NRPAL fixture")
        (aco / "shell-aliases.txt").write_bytes(b"g_from.spv g_attn.spv\n")
        (aco / "shaders/shader-constants.txt").write_bytes(b"fixture\n")
        for name in ('mochizuki-owned.txt', 'mochizuki-int4.ps1'):
            shutil.copy2(REPO / 'tools/install' / name, self.package)
        for relative in (self.package / 'mochizuki-owned.txt').read_text().splitlines():
            if not relative.strip() or relative.startswith('#'):
                continue
            path = shaders.parent / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists():
                path.write_bytes(b'pinned asset fixture')
        int4_tools(self.package / 'model-tools')
        if model:
            (shaders.parent / "dlssnr.bin").write_bytes(b"NRMODEL1" + (599).to_bytes(4, "little") + bytes(20))

    def ready_game_models(self):
        make_weights(self.game / 'native-game-tiled-assets')
        files = {
            'native-game-tiled-assets/block0-ffn.f16': b'game model marker'.ljust(17472, b'\0'),
            'native-game-tiled-assets/block1-ffn.f32': b'game model tensor'.ljust(34944, b'\0'),
            'dlssnr-amd/dlssnr.bin': b'NRMODEL1' + (599).to_bytes(4, 'little') + bytes(20),
        }
        for relative, data in files.items():
            path = self.game / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (self.game / 'dlssnr-amd/pipeline.cache').write_bytes(b'game-specific cache')
        return files

    def test_game_model_cache_all_backends_then_install_another_game(self):
        self.ready_install()
        self.ready_lmxxf_dual_arch()
        self.ready_mochizuki()
        shutil.rmtree(self.package / 'native-game-tiled-assets')
        for name in ('version.dll', 'dlssnr_on_amd_weights.bin'):
            (self.package / name).rename(self.game / name)
        expected = self.ready_game_models()
        # Installing Daniel also caches the two unselected backends' game models.
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'daniel'))
        self.assertEqual(code, 0, output)
        for relative, data in expected.items():
            self.assertEqual((self.package / relative).read_bytes(), data)
            self.assertEqual((self.game / relative).read_bytes(), data)
        self.assertEqual((self.package / 'version.dll').read_bytes(), FAKE_RUNTIME)
        self.assertEqual((self.package / 'dlssnr_on_amd_weights.bin').stat().st_size, 1024 * 1024)
        self.assertFalse((self.package / 'dlssnr-amd/pipeline.cache').exists())
        self.assertFalse(list(self.package.glob('.amd-presr-model-*')))
        other_game = self.root / 'another game'
        other_game.mkdir()
        code, output = self.run_direct(game=other_game, flags=('-NonInteractive', '-Backend', 'all', '-MochizukiModels', 'original'))
        self.assertEqual(code, 0, output)
        self.assertIn('Install SUCCEEDED.', output)
        for relative, data in expected.items():
            self.assertEqual((other_game / relative).read_bytes(), data)

    def test_game_model_cache_preserves_existing_package_models(self):
        self.ready_install()
        self.ready_game_models()
        existing = {
            'native-game-tiled-assets/user-model.bin': b'keep existing partial directory',
            'dlssnr-amd/dlssnr.bin': b'keep existing file, even if invalid',
        }
        for relative, data in existing.items():
            path = self.package / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'daniel'))
        self.assertEqual(code, 0, output)
        for relative, data in existing.items():
            self.assertEqual((self.package / relative).read_bytes(), data)
        self.assertTrue((self.package / 'native-game-tiled-assets/block0-ffn.f16').exists())
        self.assertFalse(list(self.package.glob('.amd-presr-model-*')))

    def test_small_cache_with_existing_original_installs_next_game(self):
        self.ready_mochizuki(model=True)
        small = b'NRMODEL1' + (255).to_bytes(4, 'little') + bytes(20)
        source = self.game / 'dlssnr-amd/dlssnr-small.bin'
        source.parent.mkdir(parents=True)
        source.write_bytes(small)
        (self.package / 'OptiScaler.ini').write_text('[DlssNr]\n', encoding='utf-8')
        original = (self.package / 'dlssnr-amd/dlssnr.bin').read_bytes()
        flags = ('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'both')
        code, output = self.run_direct(flags=flags)
        self.assertEqual(code, 0, output)
        self.assertEqual((self.package / 'dlssnr-amd/dlssnr-small.bin').read_bytes(), small)
        self.assertEqual((self.package / 'dlssnr-amd/dlssnr.bin').read_bytes(), original)
        other = self.root / 'next game'
        other.mkdir()
        code, output = self.run_direct(game=other, flags=flags)
        self.assertEqual(code, 0, output)
        self.assertEqual((other / 'dlssnr-amd/dlssnr-small.bin').read_bytes(), small)
        self.assertEqual((other / 'dlssnr-amd/dlssnr.bin').read_bytes(), original)
        self.assertNotIn('Extracting Mochizuki', output)
        self.assertFalse(list(self.package.glob('.amd-presr-model-*')))

    def test_small_cache_validates_and_preserves_existing_destination(self):
        self.ready_install()
        source = self.game / 'dlssnr-amd/dlssnr-small.bin'
        source.parent.mkdir(parents=True)
        source.write_bytes(b'corrupt small model')
        dest = self.package / 'dlssnr-amd/dlssnr-small.bin'
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'daniel'))
        self.assertEqual(code, 0, output)
        self.assertFalse(dest.exists())
        source.write_bytes(b'NRMODEL1' + (255).to_bytes(4, 'little') + bytes(20))
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'daniel'))
        self.assertEqual(code, 0, output)
        self.assertEqual(dest.read_bytes(), source.read_bytes())
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(b'user existing model')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'daniel'))
        self.assertEqual(code, 0, output)
        self.assertEqual(dest.read_bytes(), b'user existing model')

    def test_small_only_install_needs_no_original(self):
        self.ready_mochizuki()
        small = b'NRMODEL1' + (255).to_bytes(4, 'little') + bytes(20)
        (self.package / 'dlssnr-amd/dlssnr-small.bin').write_bytes(small)
        (self.package / 'OptiScaler.ini').write_text('[DlssNr]\n', encoding='utf-8')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'small'))
        self.assertEqual(code, 0, output)
        self.assertIn('Install SUCCEEDED.', output)
        self.assertFalse((self.game / 'dlssnr-amd/dlssnr.bin').exists())
        self.assertEqual((self.game / 'dlssnr-amd/dlssnr-small.bin').read_bytes(), small)
        self.assertRegex((self.game / 'OptiScaler.ini').read_text(encoding='utf-8-sig'), r'MochizukiNetworkModel\s*=\s*1')

    def test_both_extracts_only_missing_original_with_legacy_source(self):
        self.ready_supported_mochizuki_source()
        small = b'NRMODEL1' + (255).to_bytes(4, 'little') + bytes(20)
        existing = self.game / 'dlssnr-amd/dlssnr-small.bin'
        existing.parent.mkdir(parents=True)
        existing.write_bytes(small)
        (self.package / 'mochizuki-model.py').write_text(
            "import sys\nfrom pathlib import Path\n"
            "assert sys.argv[3:] == ['--models', 'original'], sys.argv\n"
            "p=Path(sys.argv[2]); p.parent.mkdir(parents=True,exist_ok=True)\n"
            "p.write_bytes(b'NRMODEL1'+(599).to_bytes(4,'little')+bytes(20))\n", encoding='utf-8')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'both'))
        self.assertEqual(code, 0, output)
        self.assertEqual(existing.read_bytes(), small)
        self.assertTrue((self.game / 'dlssnr-amd/dlssnr.bin').is_file())

    def test_both_extracts_only_missing_small_preserving_game_original(self):
        self.ready_supported_mochizuki_source()
        script = self.package / 'Setup.ps1'
        source_hash = hashlib.sha256((self.package / 'nvngx_dlssnr.dll').read_bytes()).hexdigest()
        write_ps(script, re.sub(r"(\$mochizukiNewNvSha\s*=\s*)'[a-f0-9]{64}'",
                               r"\g<1>'" + source_hash + "'", script.read_text(encoding='utf-8-sig')))
        original = b'NRMODEL1' + (599).to_bytes(4, 'little') + bytes(20)
        existing = self.game / 'dlssnr-amd/dlssnr.bin'
        existing.parent.mkdir(parents=True)
        existing.write_bytes(original)
        (self.package / 'mochizuki-model.py').write_text(
            "import sys\nfrom pathlib import Path\n"
            "assert sys.argv[3:] == ['--models', 'small'], sys.argv\n"
            "p=Path(sys.argv[2]).with_name('dlssnr-small.bin'); p.parent.mkdir(parents=True,exist_ok=True)\n"
            "p.write_bytes(b'NRMODEL1'+(255).to_bytes(4,'little')+bytes(20))\n", encoding='utf-8')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'both'))
        self.assertEqual(code, 0, output)
        self.assertEqual(existing.read_bytes(), original)
        self.assertTrue((self.game / 'dlssnr-amd/dlssnr-small.bin').is_file())

    def test_game_model_cache_package_is_game_skips_copy_and_cleans_temporary_runtime(self):
        self.ready_install()
        expected = self.ready_game_models()
        for relative, data in expected.items():
            path = self.package / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        code, output = self.run_direct(game=self.package, flags=('-NonInteractive', '-Backend', 'daniel'))
        self.assertEqual(code, 0, output)
        self.assertIn('Install SUCCEEDED.', output)
        for relative, data in expected.items():
            self.assertEqual((self.package / relative).read_bytes(), data)
        self.assertFalse(list(self.package.glob('.amd-presr-model-*')))
        temporary = re.search(r'Copied danielblnc runtime outside game folder: (.+)', output)
        self.assertIsNotNone(temporary, output)
        self.assertFalse(Path(temporary.group(1).strip()).exists())
        self.assertEqual((self.package / 'dlssnr_amd_pass1.dll').read_bytes(), FAKE_RUNTIME)

    def test_game_model_cache_ignores_invalid_game_models(self):
        self.ready_install()
        self.ready_game_models()
        (self.game / 'native-game-tiled-assets/block0-ffn.f16').write_bytes(b'')
        (self.game / 'dlssnr-amd/dlssnr.bin').write_bytes(b'not a model')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'daniel'))
        self.assertEqual(code, 0, output)
        self.assertIn('Install SUCCEEDED.', output)
        self.assertFalse((self.package / 'native-game-tiled-assets').exists())
        self.assertFalse((self.package / 'dlssnr-amd/dlssnr.bin').exists())

    def test_game_model_cache_failed_copy_cleans_staging_and_continues(self):
        self.ready_install()
        expected = self.ready_game_models()
        with locked_file(self.game / 'native-game-tiled-assets/block1-ffn.f32'):
            code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'daniel'))
        self.assertEqual(code, 0, output)
        self.assertIn('could not save lmxxf game model', output)
        self.assertIn('Install SUCCEEDED.', output)
        self.assertFalse((self.package / 'native-game-tiled-assets').exists())
        self.assertFalse(list(self.package.glob('.amd-presr-model-*')))
        self.assertEqual((self.package / 'dlssnr-amd/dlssnr.bin').read_bytes(),
                         expected['dlssnr-amd/dlssnr.bin'])

    def test_game_model_cache_blocked_destination_is_nonfatal(self):
        self.ready_install()
        expected = self.ready_game_models()
        (self.package / 'dlssnr-amd').write_bytes(b'preserve non-directory collision')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'daniel'))
        self.assertEqual(code, 0, output)
        self.assertIn('could not save mochizuki game model', output)
        self.assertIn('Install SUCCEEDED.', output)
        self.assertEqual((self.package / 'dlssnr-amd').read_bytes(), b'preserve non-directory collision')
        self.assertEqual((self.package / 'native-game-tiled-assets/block0-ffn.f16').read_bytes(),
                         expected['native-game-tiled-assets/block0-ffn.f16'])
        self.assertFalse(list(self.package.glob('.amd-presr-model-*')))

    def test_game_model_cache_does_not_follow_unrelated_links(self):
        self.ready_install()
        self.ready_game_models()
        outside = self.root / 'outside model'
        outside.mkdir()
        (outside / 'private.bin').write_bytes(b'do not copy')
        link = self.game / 'native-game-tiled-assets/linked'
        result = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(link), str(outside)],
                                capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.addCleanup(link.rmdir)
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'daniel'))
        self.assertEqual(code, 0, output)
        self.assertIn('Install SUCCEEDED.', output)
        self.assertTrue((self.package / 'native-game-tiled-assets/block0-ffn.f16').exists())
        self.assertFalse((self.package / 'native-game-tiled-assets/linked').exists())
        self.assertEqual((outside / 'private.bin').read_bytes(), b'do not copy')
        self.assertFalse(list(self.package.glob('.amd-presr-model-*')))

    def test_mochizuki_incomplete_aco_rejected_before_install_changes(self):
        self.ready_mochizuki()
        for relative in ("dlssnr-amd/aco/shaders/shader-constants.txt", "dlssnr-amd/aco/records",
                         "dlssnr-amd/aco/shell-aliases.txt", "dlssnr-amd/aco/int4/records",
                         "dlssnr-amd/int4/settings.txt", "dlssnr-amd/shaders/renodx",
                         "model-tools/dlssnr-int4-weights.exe", "mochizuki-int4.ps1"):
            path = self.package / relative
            saved = self.root / "removed-aco-asset"
            if saved.exists():
                if saved.is_dir():
                    shutil.rmtree(saved)
                else:
                    saved.unlink()
            path.rename(saved)
            code, output = self.run_direct(flags=("-NonInteractive", "-Backend", "mochizuki"))
            self.assertNotEqual(code, 0, output)
            self.assertIn("Incomplete Mochizuki package", output)
            self.assertFalse((self.game / "MochizukiNrRuntime.dll").exists(), output)
            saved.rename(path)

    def test_mochizuki_missing_model_fails_and_preserves_game(self):
        self.ready_mochizuki()
        before = self.game_snapshot()
        code, output = self.run_direct(flags=("-NonInteractive", "-Backend", "mochizuki", "-MochizukiModels", "original"))
        self.assertEqual(code, 1, output)
        self.assertIn("The requested Mochizuki model selection is unavailable", output)
        self.assertNotIn("Install SUCCEEDED.", output)
        self.assertFalse((self.game / "MochizukiNrRuntime.dll").exists())
        self.assertEqual(self.game_snapshot(), before)

    def test_mochizuki_noninteractive_requires_models_choice(self):
        self.ready_mochizuki(model=True)
        before = self.game_snapshot()
        code, output = self.run_direct(flags=("-NonInteractive", "-Backend", "mochizuki"))
        self.assertEqual(code, 1, output)
        self.assertIn("MochizukiModels parameter is required in -NonInteractive mode", output)
        self.assertNotIn("Install SUCCEEDED.", output)
        self.assertEqual(self.game_snapshot(), before)

    def test_unselected_mochizuki_never_prepares_weights(self):
        self.ready_install()
        self.ready_lmxxf_dual_arch()
        self.ready_mochizuki(model=True)
        # Loading this helper itself must be deferred, not just the converter call.
        (self.package / 'mochizuki-int4.ps1').write_text("throw 'UNSELECTED_MOCHI_HELPER'", encoding='utf-8-sig')
        for backend in ('lmxxf', 'daniel'):
            with self.subTest(backend=backend):
                code, output = self.run_direct(flags=('-NonInteractive', '-Backend', backend))
                self.assertEqual(code, 0, output)
                self.assertNotIn('UNSELECTED_MOCHI_HELPER', output)
                self.assertNotIn('Preparing INT4', output)
                self.assertNotIn('DLL is not supported', output)
                self.assertFalse((self.game / 'dlssnr-amd/dlssnr-int4.bin').exists())

    def test_interactive_backend_choice_precedes_int4(self):
        self.ready_mochizuki(model=True)
        args = [str(PS), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                str(self.package / 'Setup.ps1'), '-GameDir', str(self.game), '-Proxy', 'dxgi.dll', '-NoPause']
        # '2' chooses mochizuki backend; '1' confirms original model under legacy warning
        code, output = self.run_process(args, '2\n1\n')
        self.assertEqual(code, 0, output)
        self.assertLess(output.index('Choose NR backends to install:'), output.index('Preparing INT4'))
        self.assertEqual(output.count('Preparing INT4'), 1, output)
        self.assertTrue((self.game / 'dlssnr-amd/dlssnr-int4.bin').is_file())

    def test_mochizuki_interactive_cancel_preserves_game(self):
        self.ready_supported_mochizuki_source()
        before = self.game_snapshot()
        args = [str(PS), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                str(self.package / 'Setup.ps1'), '-GameDir', str(self.game),
                '-Proxy', 'dxgi.dll', '-Backend', 'mochizuki', '-NoPause']
        # '0' cancels at model selection prompt
        code, output = self.run_process(args, '0\n')
        self.assertEqual(code, 0, output)
        self.assertIn('Cancelled.', output)
        self.assertNotIn('Install SUCCEEDED.', output)
        self.assertFalse((self.game / 'MochizukiNrRuntime.dll').exists())
        self.assertEqual(self.game_snapshot(), before)

    def test_existing_mochizuki_model_ignores_unneeded_nvidia_dll(self):
        self.ready_mochizuki(model=True)
        (self.game / 'nvngx_dlssnr.dll').write_bytes(b'unrelated DLL version')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'original'))
        self.assertEqual(code, 0, output)
        self.assertNotIn('DLL is not supported', output)
        self.assertNotIn('Extracting Mochizuki model', output)
        self.assertIn('Install SUCCEEDED.', output)

    def test_mochizuki_existing_model_needs_no_extraction(self):
        self.ready_mochizuki(model=True)
        expected = (self.package / "dlssnr-amd/dlssnr.bin").read_bytes()
        code, output = self.run_direct(flags=("-NonInteractive", "-Backend", "mochizuki", "-MochizukiModels", "original"))
        self.assertEqual(code, 0, output)
        self.assertIn("Install SUCCEEDED.", output)
        self.assertNotIn("Extracting Mochizuki model", output)
        self.assertEqual((self.game / "dlssnr-amd/dlssnr.bin").read_bytes(), expected)
        self.assertEqual((self.game / "dlssnr-amd/dlssnr-int4.bin").read_bytes(), INT4_PAYLOAD)

    def test_mochizuki_installs_both_models_and_preserves_selection_on_upgrade(self):
        self.ready_mochizuki(model=True)
        small = b'NRMODEL1' + (255).to_bytes(4, 'little') + bytes(20)
        (self.package / 'dlssnr-amd/dlssnr-small.bin').write_bytes(small)
        ini = self.game / 'OptiScaler.ini'
        ini.write_text('[DlssNr]\nBackend=mochizuki\nMochizukiNetworkModel=0\nMochizukiInt4Mixed=true\n', encoding='utf-8')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'both'))
        self.assertEqual(code, 0, output)
        self.assertEqual((self.game / 'dlssnr-amd/dlssnr-small.bin').read_bytes(), small)
        self.assertRegex(ini.read_text(encoding='utf-8-sig'), r'MochizukiNetworkModel\s*=\s*0')
        self.assertIn('MochizukiInt4Mixed=true', ini.read_text(encoding='utf-8-sig'))
        (self.package / 'dlssnr-amd/dlssnr-small.bin').write_bytes(small + b'package copy changed')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'both'))
        self.assertEqual(code, 0, output)
        self.assertEqual((self.game / 'dlssnr-amd/dlssnr-small.bin').read_bytes(), small)

    def test_mochizuki_invalid_small_model_is_not_copied(self):
        self.ready_mochizuki(model=True)
        (self.package / 'dlssnr-amd/dlssnr-small.bin').write_bytes(b'NRMODEL1' + (599).to_bytes(4, 'little') + bytes(20))
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'original'))
        self.assertEqual(code, 0, output)
        self.assertFalse((self.game / 'dlssnr-amd/dlssnr-small.bin').exists())
        self.assertTrue((self.game / 'dlssnr-amd/dlssnr.bin').is_file())

    def test_launcher_update_reuses_game_models_after_package_cache_deleted(self):
        self.ready_mochizuki(model=True)
        self.ready_lmxxf_dual_arch()
        # Synthetic Small inventory exercises production copy/validation without real weights.
        tensor = b'lmxxf Small fixture'
        manifest_path = self.package / 'lmxxf-small-manifest.json'
        manifest = json.loads(manifest_path.read_text())
        manifest['files'] = {'fixture.f16': dict(bytes=len(tensor), sha256=hashlib.sha256(tensor).hexdigest())}
        manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
        small_root = self.package / 'native-game-tiled-assets/small'
        small_root.mkdir()
        (small_root / 'fixture.f16').write_bytes(tensor)
        (small_root / 'model-manifest.json').write_text(json.dumps(dict(model='small',
            source_dll_sha256=manifest['source_dll_sha256'])), encoding='utf-8')
        small = b'NRMODEL1' + (255).to_bytes(4, 'little') + bytes(20)
        (self.package / 'dlssnr-amd/dlssnr-small.bin').write_bytes(small)
        # Substitute only the canonical source pin; keep real hash/worker/install paths.
        helper = self.package / 'mochizuki-int4.ps1'
        canonical = hashlib.sha256((self.package / 'dlssnr-amd/dlssnr.bin').read_bytes()).hexdigest()
        source = helper.read_text(encoding='utf-8-sig')
        self.assertEqual(source.count('2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd'), 1)
        write_ps(helper, source.replace('2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd', canonical))
        decisions = self.root / 'initial-models.json'
        decisions.write_text(json.dumps(dict(backends=['mochizuki', 'lmxxf'], existingIni='keep',
            mochizukiModels='both', lmxxfModels='both')), encoding='utf-8')
        code, output = self.run_direct(flags=('-NonInteractive', '-DecisionJson', str(decisions)))
        self.assertEqual(code, 0, output)
        self.assertIn('Preparing INT4 weights', output, 'Fresh installs must still generate INT4')
        models = [self.game / ('dlssnr-amd/' + name) for name in ('dlssnr.bin', 'dlssnr-small.bin', 'dlssnr-int4.bin')]
        models += [self.game / 'native-game-tiled-assets/small' / name for name in ('fixture.f16', 'model-manifest.json')]
        before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in models}
        for name in ('dlssnr.bin', 'dlssnr-small.bin'):
            (self.package / 'dlssnr-amd' / name).unlink()
        shutil.rmtree(self.package / 'native-game-tiled-assets')
        # A converter invocation is now fatal: a successful update proves real reuse.
        (self.package / 'model-tools/dlssnr-int4-weights.exe').write_bytes(b'converter must not run')
        (self.package / 'OptiScaler.dll').write_bytes(b'version-mismatched package proxy')
        exe = self.game / 'reuse-game.exe'
        shutil.copy2(Path(os.environ['SystemRoot']) / 'System32/whoami.exe', exe)
        for name in ('launcher-worker.ps1', 'proxy-probe.ps1', 'install-capabilities.ps1'):
            shutil.copy2(REPO / 'tools/install' / name, self.package)
        request = self.root / 'request.json'
        request.write_text(json.dumps(dict(schema=1, requestId='reuse-update', action='update',
            gameExe=str(exe), proxyChoice='dxgi.dll', packageRoot=str(self.package),
            backends=['mochizuki', 'lmxxf'], activeBackend='mochizuki', explicitDecisions=dict(
                existingInstall='overwrite', existingIni='keep', mochizukiModels='both', lmxxfModels='both',
                foreignProxy={'dxgi.dll': 'backup'}))), encoding='utf-8')
        result = self.root / 'result.json'
        events = self.root / 'events.jsonl'
        code, output = self.run_process([str(PS), '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
            '-File', str(self.package / 'launcher-worker.ps1'), '-RequestJson', str(request),
            '-EventsJsonl', str(events), '-ResultJson', str(result)], timeout=120)
        body = json.loads(result.read_text(encoding='utf-8-sig'))
        log = events.read_text(encoding='utf-8-sig')
        self.assertEqual(code, 0, output + log)
        self.assertEqual(body['status'], 'success', body)
        self.assertIn('INT4 weights already match', log)
        self.assertNotIn('Preparing INT4 weights', log)
        self.assertNotIn('Extracting Mochizuki', log)
        self.assertNotIn('Preparing lmxxf', log)
        self.assertEqual((self.game / 'dxgi.dll').read_bytes(), b'version-mismatched package proxy')
        self.assertEqual({p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in models}, before)

    def test_mochizuki_int4_reuse_and_failed_conversion_preserve_weights(self):
        self.ready_mochizuki(model=True)
        flags = ('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'original')
        code, output = self.run_direct(flags=flags)
        self.assertEqual(code, 0, output)
        output_model = self.game / 'dlssnr-amd/dlssnr-int4.bin'
        first_time = output_model.stat().st_mtime_ns
        code, output = self.run_direct(flags=flags)
        self.assertEqual(code, 0, output)
        self.assertIn('INT4 weights already match', output)
        self.assertIn('Preparing INT4 weights', output, 'Noncanonical sources must still be converted')
        self.assertEqual(output_model.stat().st_mtime_ns, first_time)
        (self.package / 'model-tools/dlssnr-int4.sha256').write_text('0' * 64)
        before = self.game_snapshot()
        code, output = self.run_direct(flags=flags)
        self.assertNotEqual(code, 0, output)
        self.assertIn('does not match the version this package expects', output)
        self.assertEqual(output_model.read_bytes(), INT4_PAYLOAD)
        self.assertEqual(self.game_snapshot(), before, output)
        self.assertFalse(list(output_model.parent.glob('.mochizuki-int4-*.tmp')))

    def game_snapshot(self):
        return {p.relative_to(self.game).as_posix(): p.read_bytes() if p.is_file() else None
                for p in sorted(self.game.rglob('*'))}

    def test_weights_check_all_skips_failed_mochizuki_and_installs_others(self):
        self.ready_install()
        self.ready_mochizuki(model=True)
        (self.package / 'model-tools/dlssnr-int4.sha256').write_text('ab' * 32 + '\n')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'all', '-MochizukiModels', 'original'))
        self.assertEqual(code, 0, output)
        self.assertIn('WEIGHTS CHECK: problems found', output)
        self.assertIn('<mochizuki: FAILED - this backend will not be installed', output)
        self.assertIn('Skipping mochizuki: weights check failed', output)
        self.assertIn('WEIGHTS CHECK mochizuki: FAILED, not installed', output)  # end-of-run summary
        self.assertLess(output.rindex('WEIGHTS CHECK mochizuki'), output.rindex('Install SUCCEEDED.'))
        self.assertEqual((self.game / 'dlssnr_amd_pass1.dll').read_bytes(), FAKE_RUNTIME)
        self.assertFalse((self.game / 'MochizukiNrRuntime.dll').exists(), output)
        self.assertFalse((self.game / 'dlssnr-amd').exists(), output)

    def test_weights_check_mochizuki_prefers_matching_model_over_stale_game_copy(self):
        self.ready_mochizuki(model=True)
        stale = self.game / 'dlssnr-amd/dlssnr.bin'
        stale.parent.mkdir()
        stale.write_bytes(b'NRMODEL1' + (599).to_bytes(4, 'little') + b'stale model fixture')
        converter = self.package / 'model-tools/dlssnr-int4-weights.exe'
        script = self.package / 'mochizuki-int4.ps1'
        # Fixture converter output is model-independent; make the stale game copy fail by path.
        text = script.read_text(encoding='utf-8-sig').replace(
            "        & $generator $Model $Data $temporary | Out-Host",
            "        & $generator $Model $Data $temporary | Out-Host\n"
            "        if ($Model -like '*game with spaces*') { [IO.File]::WriteAllText($temporary, 'stale') }")
        script.write_text(text, encoding='utf-8-sig')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'original'))
        self.assertEqual(code, 0, output)
        self.assertIn('Wrong file: ' + str(stale), output)
        self.assertIn('Setup will use the matching model instead: ' + str(self.package / 'dlssnr-amd' / 'dlssnr.bin'), output)
        self.assertIn('<mochizuki: warning', output)
        self.assertEqual(stale.read_bytes(), (self.package / 'dlssnr-amd/dlssnr.bin').read_bytes())
        aside = list(stale.parent.glob('dlssnr.bin.mismatch-*'))
        self.assertEqual(len(aside), 1, output)
        self.assertTrue(aside[0].read_bytes().endswith(b'stale model fixture'))
        self.assertEqual((self.game / 'dlssnr-amd/dlssnr-int4.bin').read_bytes(), INT4_PAYLOAD)

    def test_weights_check_truncated_daniel_weights_fail_untouched(self):
        self.ready_install()
        (self.package / 'dlssnr_on_amd_weights.bin').write_bytes(bytes(1000))
        before = self.game_snapshot()
        for backend in ('daniel', 'all'):
            with self.subTest(backend=backend):
                code, output = self.run_direct(flags=('-NonInteractive', '-Backend', backend))
                self.assertNotEqual(code, 0, output)
                self.assertIn('[daniel] Wrong file: ' + str(self.package / 'dlssnr_on_amd_weights.bin'), output)
                self.assertIn('only 1000 bytes', output)
                self.assertIn('Weights check failed for: daniel. Nothing was installed', output)
                self.assertEqual(self.game_snapshot(), before, output)

    def test_weights_check_incomplete_lmxxf_folder_warns_red(self):
        self.ready_lmxxf_dual_arch()
        weights = self.package / 'native-game-tiled-assets'
        (weights / 'block0-ffn.f16').unlink()
        (weights / 'other.bin').write_bytes(b'unrelated')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'lmxxf'))
        self.assertEqual(code, 0, output)
        self.assertIn('WEIGHTS CHECK: problems found', output)
        self.assertIn('[lmxxf] Wrong folder: ' + str(weights), output)
        self.assertIn('<lmxxf: warning', output)
        self.assertIn('WEIGHTS CHECK lmxxf: warning', output)
        self.assertIn('MODEL SETUP REQUIRED', output)

    def test_mochizuki_int4_mismatch_leaves_game_untouched(self):
        self.ready_mochizuki(model=True)
        (self.game / 'game.exe').write_bytes(b'game executable fixture')
        (self.game / 'dxgi.dll').write_bytes(b'original proxy fixture')
        (self.package / 'model-tools/dlssnr-int4.sha256').write_text('ab' * 32 + '\n')
        before = self.game_snapshot()
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'original'))
        self.assertNotEqual(code, 0, output)
        self.assertEqual(self.game_snapshot(), before, output)
        for text in ('WEIGHTS CHECK: problems found',
                     'The Mochizuki model you supplied does not match the version this package expects.',
                     'Wrong file: ' + str(self.package / 'dlssnr-amd' / 'dlssnr.bin'),
                     'nvngx_dlssnr.dll 310.8.0; INT4 weights SHA-256 ' + 'ab' * 32,
                     'Weights check failed for: mochizuki. Nothing was installed; the game folder was not changed.',
                     'run Mochizuki-Model.bat, then run Setup again.', 'Install FAILED.'):
            self.assertIn(text, output)
        self.assertNotIn('Install SUCCEEDED.', output)
        self.assertFalse(list(self.package.rglob('.mochizuki-int4-*.tmp')))

    def test_mochizuki_reuses_game_model(self):
        self.ready_mochizuki()
        model = self.game / 'dlssnr-amd/dlssnr.bin'
        model.parent.mkdir()
        data = b'NRMODEL1' + (599).to_bytes(4, 'little') + bytes(20)
        model.write_bytes(data)
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'original'))
        self.assertEqual(code, 0, output)
        self.assertIn('Install SUCCEEDED.', output)
        self.assertNotIn('Extracting Mochizuki model', output)
        self.assertEqual(model.read_bytes(), data)

    def test_mochizuki_invalid_game_model_is_preserved_and_reported(self):
        self.ready_mochizuki(model=True)
        model = self.game / 'dlssnr-amd/dlssnr.bin'
        model.parent.mkdir()
        model.write_bytes(b'user invalid model')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'original'))
        self.assertEqual(code, 0, output)
        self.assertIn('Install SUCCEEDED.', output)
        self.assertIn('Backed up mismatched or invalid model', output)
        self.assertEqual(model.read_bytes(), (self.package / 'dlssnr-amd/dlssnr.bin').read_bytes())
        backups = list(model.parent.glob('dlssnr.bin.mismatch-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), b'user invalid model')

    def test_mochizuki_unsupported_nvidia_dll_fails_and_preserves_game(self):
        self.ready_mochizuki()
        (self.package / 'nvngx_dlssnr.dll').write_bytes(b'unsupported model DLL')
        before = self.game_snapshot()
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'original'))
        self.assertEqual(code, 1, output)
        self.assertIn('Wrong file: ', output)
        self.assertIn('The requested Mochizuki model selection is unavailable', output)
        self.assertNotIn('Install SUCCEEDED.', output)
        self.assertEqual(self.game_snapshot(), before)

    def ready_supported_mochizuki_source(self):
        self.ready_mochizuki()
        source = b'fixture supported model source'
        (self.package / 'nvngx_dlssnr.dll').write_bytes(source)
        script = self.package / 'Setup.ps1'
        text = script.read_text(encoding='utf-8-sig')
        old = 'e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e'
        self.assertEqual(text.lower().count(old), 1)
        text = re.sub(old, hashlib.sha256(source).hexdigest(), text, flags=re.IGNORECASE)
        write_ps(script, text)

    def test_mochizuki_supported_source_extracts_and_installs(self):
        self.ready_supported_mochizuki_source()
        (self.package / 'mochizuki-model.py').write_text(
            "import sys\nfrom pathlib import Path\np=Path(sys.argv[2]); p.parent.mkdir(parents=True,exist_ok=True)\n"
            "p.write_bytes(b'NRMODEL1'+(599).to_bytes(4,'little')+bytes(20))\n", encoding='utf-8')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'original'))
        self.assertEqual(code, 0, output)
        self.assertIn('Extracting Mochizuki models', output)
        self.assertIn('Install SUCCEEDED.', output)
        self.assertTrue((self.game / 'dlssnr-amd/dlssnr.bin').is_file())

    def test_mochizuki_extractor_failure_leaves_actionable_pending(self):
        self.ready_supported_mochizuki_source()
        (self.package / 'mochizuki-model.py').write_text('import sys; print("fixture extraction error", file=sys.stderr); sys.exit(4)\n', encoding='utf-8')
        before = self.game_snapshot()
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'original'))
        self.assertEqual(code, 1, output)
        self.assertIn('Model preparation failed', output)
        self.assertIn('The existing installation was preserved', output)
        self.assertNotIn('Install SUCCEEDED.', output)
        self.assertEqual(self.game_snapshot(), before)

    def test_mochizuki_missing_python_leaves_actionable_pending(self):
        self.ready_supported_mochizuki_source()
        script = self.package / 'mochizuki-python.ps1'
        text = script.read_text(encoding='utf-8-sig')
        old = "@('python', 'python3', 'py')"
        self.assertEqual(text.count(old), 1)
        write_ps(script, text.replace(old, "@('nonexistent-python-fixture')"))
        before = self.game_snapshot()
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'original'))
        self.assertEqual(code, 1, output)
        self.assertIn('Python 3.10+', output)
        self.assertIn('Microsoft Store (Windows Store)', output)
        self.assertIn('The existing installation was preserved', output)
        self.assertNotIn('Extracting Mochizuki model', output)
        self.assertEqual(self.game_snapshot(), before)

    def fixture_python_candidates(self, candidates):
        script = self.package / 'mochizuki-python.ps1'
        text = script.read_text(encoding='utf-8-sig')
        old = "@('python', 'python3', 'py')"
        self.assertEqual(text.count(old), 1)
        paths = ', '.join("'" + str(p).replace("'", "''") + "'" for p in candidates)
        write_ps(script, text.replace(old, '@(' + paths + ')'))

    def test_mochizuki_unusable_python_fails_and_preserves_game(self):
        self.ready_supported_mochizuki_source()
        broken = self.package / 'broken-python.cmd'
        broken.write_text('@echo off\necho Python was not found 1>&2\nexit /b 1\n', encoding='ascii')
        empty = self.package / 'empty-python.cmd'
        empty.write_text('@echo off\nexit /b 0\n', encoding='ascii')
        old = self.package / 'old-python.cmd'
        old.write_text('@echo off\necho MOCHI_PYTHON_TOO_OLD\nexit /b 0\n', encoding='ascii')
        self.fixture_python_candidates((broken, empty, old))
        before = self.game_snapshot()
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'original'))
        self.assertEqual(code, 1, output)
        self.assertIn('Python 3.10+', output)
        self.assertIn('The existing installation was preserved', output)
        self.assertNotIn('Install SUCCEEDED.', output)
        self.assertEqual(self.game_snapshot(), before)

    def test_mochizuki_python_fallback_after_broken_launcher(self):
        self.ready_supported_mochizuki_source()
        broken = self.package / 'broken-python.cmd'
        broken.write_text('@echo off\necho launcher error 1>&2\nexit /b 1\n', encoding='ascii')
        self.fixture_python_candidates((broken, Path(sys.executable)))
        (self.package / 'mochizuki-model.py').write_text(
            "import sys\nfrom pathlib import Path\np=Path(sys.argv[2]); p.parent.mkdir(parents=True,exist_ok=True)\n"
            "p.write_bytes(b'NRMODEL1'+(599).to_bytes(4,'little')+bytes(20))\n", encoding='utf-8')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'original'))
        self.assertEqual(code, 0, output)
        self.assertIn('Install SUCCEEDED.', output)
        self.assertNotIn('search for Python', output)

    def test_mochizuki_same_name_python_fallback(self):
        self.ready_supported_mochizuki_source()
        first, second = self.root / 'first-bin', self.root / 'second-bin'
        first.mkdir(); second.mkdir()
        (first / 'fixture-python.cmd').write_text('@echo off\necho launcher error 1>&2\nexit /b 1\n', encoding='ascii')
        (second / 'fixture-python.cmd').write_text('@echo off\n"' + sys.executable + '" %*\nexit /b %errorlevel%\n', encoding='ascii')
        self.env['PATH'] = str(first) + os.pathsep + str(second) + os.pathsep + self.env['PATH']
        self.fixture_python_candidates(('fixture-python',))
        (self.package / 'mochizuki-model.py').write_text(
            "import sys\nfrom pathlib import Path\np=Path(sys.argv[2]); p.parent.mkdir(parents=True,exist_ok=True)\n"
            "p.write_bytes(b'NRMODEL1'+(599).to_bytes(4,'little')+bytes(20))\n", encoding='utf-8')
        code, output = self.run_direct(flags=('-NonInteractive', '-Backend', 'mochizuki', '-MochizukiModels', 'original'))
        self.assertEqual(code, 0, output)
        self.assertIn('Install SUCCEEDED.', output)
        self.assertNotIn('search for Python', output)

    def run_mochizuki_model_launcher(self, stdin="1\n"):
        source = (REPO / 'tools/release/PACKAGE_RELEASE.ps1').read_text(encoding='utf-8-sig')
        body = re.search(r"@'\n(@echo off\nsetlocal\nset \"SOURCE=%~1\".*?)\n'@ \| Set-Content", source, re.S)
        self.assertIsNotNone(body)
        launcher = self.package / 'Mochizuki-Model.bat'
        launcher.write_text(body.group(1), encoding='ascii')
        code, output = self.run_process('cmd.exe /d /s /c "' + subprocess.list2cmdline([str(launcher)]) + '"', stdin)
        return code, output

    def test_mochizuki_model_launcher_missing_python(self):
        self.fixture_python_candidates(('nonexistent-python-fixture',))
        code, output = self.run_mochizuki_model_launcher()
        self.assertNotEqual(code, 0, output)
        self.assertIn('search for Python', output)
        self.assertIn('Microsoft Store', output)

    def test_mochizuki_model_launcher_preserves_extractor_error(self):
        self.fixture_python_candidates((Path(sys.executable),))
        (self.package / 'mochizuki-model.py').write_text('import sys; print("fixture extraction error"); sys.exit(4)\n', encoding='utf-8')
        code, output = self.run_mochizuki_model_launcher()
        self.assertEqual(code, 4, output)
        self.assertIn('fixture extraction error', output)
        self.assertIn('Python was found', output)
        self.assertNotIn('search for Python', output)

    def test_lmxxf_empty_weight_directory_is_not_ready(self):
        self.ready_lmxxf_dual_arch()
        (self.package / "native-game-tiled-assets/block0-ffn.f16").unlink()
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("MODEL SETUP REQUIRED", output)
        self.assertNotIn("Install SUCCEEDED.", output)

    def test_lmxxf_empty_weight_marker_is_not_ready(self):
        self.ready_lmxxf_dual_arch()
        (self.package / "native-game-tiled-assets/block0-ffn.f16").write_bytes(b"")
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("MODEL SETUP REQUIRED", output)
        self.assertNotIn("Install SUCCEEDED.", output)

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
        self.assertEqual(len(list((game_mods / "gfx1200").glob("*.hsaco"))), 40)
        self.assertEqual(len(list((game_mods / "gfx1201").glob("*.hsaco"))), 40)
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
        self.add_legacy_shader_files(mods)
        return mods

    def add_legacy_shader_files(self, mods):
        for relative in ('shaders/native_game_rgb_input.hlsl', 'shaders/preblock_retired.hlsl',
                         'shaders/shader-cache/213cc4c0f37d5145.dxbc',
                         'shaders/shader-cache/2159d4b8f3f84637.v2.dxbc.12.3.tmp'):
            path = mods / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'old installed shader or cache')
        (mods / 'shaders/user-custom.hlsl').write_bytes(b'keep custom shader')
        (mods / 'shaders/shader-cache/notes.txt').write_bytes(b'keep cache notes')

    def assert_legacy_shaders_removed(self, mods):
        self.assertFalse((mods / 'shaders/native_game_rgb_input.hlsl').exists())
        self.assertFalse((mods / 'shaders/preblock_retired.hlsl').exists())
        self.assertEqual(list((mods / 'shaders/shader-cache').iterdir()),
                         [mods / 'shaders/shader-cache/notes.txt'])
        self.assertEqual((mods / 'shaders/user-custom.hlsl').read_bytes(), b'keep custom shader')

    def assert_dual_arch_installed(self):
        mods = self.game / "lmxxf-modules"
        self.assertFalse(list(mods.glob("*.hsaco")))
        self.assertFalse((mods / "modules.json").exists())
        for arch in ("gfx1200", "gfx1201"):
            self.assertEqual(len(list((mods / arch).glob("*.hsaco"))), 40)
        return mods

    def assert_custom_module_backed_up(self):
        saved = list(self.game.glob("backup-amd-presr-*/lmxxf-modules/user-custom.hsaco"))
        self.assertEqual(len(saved), 1)
        self.assertEqual(saved[0].read_bytes(), b"user-owned GPU code")

    def test_legacy_upgrade_preserves_only_excluded_user_modules(self):
        mods = self.ready_legacy_install()
        before = snapshot_files(mods)
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        self.assertIn("Install SUCCEEDED", output)
        self.assertNotIn("Uninstalling the existing", output)
        self.assert_dual_arch_installed()
        self.assert_custom_module_backed_up()
        backups = list(self.game.glob("backup-amd-presr-*/lmxxf-modules"))
        self.assertEqual(snapshot_files(backups[0]), {"user-custom.hsaco": before["user-custom.hsaco"]})
        self.assertFalse(list(self.game.glob(".lmxxf-previous-*")))
        self.assertEqual((mods / "user_weights.bin").read_bytes(), b"user weights")
        self.assert_legacy_shaders_removed(mods)

    def test_clean_overwrite_has_no_permanent_or_temporary_old_package(self):
        self.ready_lmxxf_dual_arch()
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        mods = self.game / "lmxxf-modules"
        (mods / "c32_fast.generated.hip").write_bytes(b"obsolete source")
        (mods / "gfx1201/c32_fast.hsaco.s").write_bytes(b"obsolete disassembly")
        (mods / "user-notes.txt").write_bytes(b"keep notes")
        self.add_legacy_shader_files(mods)
        (self.game / "LmxxfNrRuntime.dll").write_bytes(b"previous runtime")
        with (self.game / "OptiScaler.ini").open("a", encoding="utf-8") as stream:
            stream.write("\n[User]\nSentinel=keep\n")
        for _ in range(2):
            code, output = self.run_direct()
            self.assertEqual(code, 0, output)
            self.assertFalse(list(self.game.glob("backup-amd-presr-*")))
            self.assertFalse(list(self.game.glob(".lmxxf-previous-*")))
            self.assertFalse(list(self.game.glob(".lmxxf-stage-*")))
            self.assertEqual((mods / "user-notes.txt").read_bytes(), b"keep notes")
            self.assertFalse((mods / "c32_fast.generated.hip").exists())
            self.assertFalse((mods / "gfx1201/c32_fast.hsaco.s").exists())
            self.assertEqual((self.game / "LmxxfNrRuntime.dll").read_bytes(),
                             (self.package / "LmxxfNrRuntime.dll").read_bytes())
            self.assertIn("Sentinel=keep", (self.game / "OptiScaler.ini").read_text(encoding="utf-8-sig"))
            self.assert_dual_arch_installed()
            self.assert_legacy_shaders_removed(mods)

    def test_setup_no_overwrites_legacy_without_uninstall_or_second_proxy_prompt(self):
        self.ready_legacy_install()
        code, output = self.run_batch(stdin="n\n3\n2\n")
        self.assertEqual(code, 0, output)
        self.assertEqual(output.count("Existing OptiScaler installation detected"), 1, output)
        self.assertIn("Recommended: uninstall before installing this version", output)
        self.assertIn("Continuing with an overwrite installation", output)
        self.assertNotIn("Uninstalling the existing", output)
        self.assertNotIn("How to continue?", output)
        self.assert_dual_arch_installed()
        self.assert_custom_module_backed_up()
        self.assertIn("UserSentinel", (self.game / "OptiScaler.ini").read_text(encoding="utf-8-sig"))

    def test_old_and_new_brand_records_detect_install_without_proxy(self):
        self.ready_install()
        record = self.game / "amd-presr-install.txt"
        for brand in ("OptiScaler AMD pre-SR", "OptScaler(NR)"):
            with self.subTest(brand=brand):
                (self.game / "dxgi.dll").unlink(missing_ok=True)
                record.write_text(f"project={brand}\nproxy=dxgi.dll\n", encoding="ascii")
                code, output = self.run_batch(stdin="N\n")
                self.assertEqual(code, 0, output)
                self.assertIn("Existing OptiScaler installation detected", output)
                self.assertIn("Continuing with an overwrite installation", output)
                self.assertIn("project=OptScaler(NR)", record.read_text())

    def test_setup_yes_uses_new_uninstaller_then_installs_without_second_confirmation(self):
        self.ready_legacy_install()
        (self.package / "OptiScaler.ini").write_text("[DlssNr]\nPackageSentinel=yes\n", encoding="utf-8")
        (self.game / "dlssnr_amd_pass3.dll").write_bytes(b"old unused backend")
        write_ps(self.game / "Uninstall_OptiScaler_NR.ps1", "throw 'old uninstaller must not run'\n")
        backup = self.game / "backup-amd-presr-existing"
        backup.mkdir()
        (backup / "keep.bin").write_bytes(b"old backup")
        code, output = self.run_batch(stdin="Y\n3\n1\n")
        self.assertEqual(code, 0, output)
        self.assertIn("Uninstall SUCCEEDED.", output)
        self.assertIn("Install SUCCEEDED.", output)
        self.assertLess(output.index("Uninstall SUCCEEDED."), output.index("Installing OptScaler(NR)"))
        self.assertNotIn("Type Y to delete", output)
        self.assertNotIn("Keep these backup folders?", output)
        self.assertNotIn("old uninstaller must not run", output)
        self.assert_uninstaller_path(output, self.package / 'Uninstall_OptiScaler_NR.ps1')
        self.assertIn('uninstallFirst=true', (self.game / 'amd-presr-install.txt').read_text())
        self.assertFalse((self.game / "dlssnr_amd_pass3.dll").exists())
        self.assertEqual((backup / "keep.bin").read_bytes(), b"old backup")
        self.assert_dual_arch_installed()
        self.assert_custom_module_backed_up()
        ini = (self.game / "OptiScaler.ini").read_text(encoding="utf-8-sig")
        self.assertIn("PackageSentinel", ini)
        self.assertNotIn("UserSentinel", ini)
        self.assert_legacy_shaders_removed(self.game / 'lmxxf-modules')

    def test_release_uninstaller_wins_over_stale_root_copy(self):
        self.ready_legacy_install()
        release = self.package / 'release'
        release.mkdir()
        (self.package / 'OptiScaler.dll').rename(release / 'OptiScaler.dll')
        shutil.copy2(self.package / 'Uninstall_OptiScaler_NR.ps1', release / 'Uninstall_OptiScaler_NR.ps1')
        write_ps(self.package / 'Uninstall_OptiScaler_NR.ps1', "throw 'stale root uninstaller ran'\n")
        code, output = self.run_batch(stdin='Y\n3\n1\n')
        self.assertEqual(code, 0, output)
        self.assert_uninstaller_path(output, release / 'Uninstall_OptiScaler_NR.ps1')
        self.assertNotIn('stale root uninstaller ran', output)
        self.assertIn('Uninstall SUCCEEDED.', output)
        self.assert_legacy_shaders_removed(self.game / 'lmxxf-modules')

    def test_existing_install_prompt_precedes_proxy_menu(self):
        self.ready_legacy_install()
        args = [str(PS), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                str(self.package / "Setup.ps1"), "-GameDir", str(self.game), "-NoPause"]
        # Overwrite installation, dxgi proxy, Original model, then overwrite INI.
        code, output = self.run_process(args, "NO\n1\n3\n1\n")
        self.assertEqual(code, 0, output)
        self.assertLess(output.index("Existing OptiScaler installation detected"), output.index("Which proxy DLL"))

    def test_setup_overwrite_moves_other_opti_proxy_without_reprompt(self):
        self.ready_legacy_install()
        (self.game / "dxgi.dll").rename(self.game / "winmm.dll")
        code, output = self.run_batch(stdin="N\n3\n1\n")
        self.assertEqual(code, 0, output)
        self.assertFalse((self.game / "winmm.dll").exists())
        self.assertEqual((self.game / "dxgi.dll").read_bytes(), b"fixture proxy")
        self.assertFalse(list(self.game.glob("backup-amd-presr-*/winmm.dll*")))
        self.assertNotIn("How to continue?", output)

    def test_setup_uninstall_failure_stops_before_install(self):
        self.ready_legacy_install()
        write_ps(self.package / "Uninstall_OptiScaler_NR.ps1",
                 "param($GameDir, [switch]$NonInteractive, [switch]$NoPause)\nexit 7\n")
        before = snapshot_files(self.game)
        code, output = self.run_batch(stdin="Y\n3\n1\n")
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

    def test_setup_clean_reinstall_rejects_missing_update_components(self):
        self.ready_lmxxf_dual_arch()
        code, output = self.run_direct()
        self.assertEqual(code, 0, output)
        before = snapshot_files(self.game)
        for relative in ('LmxxfNrRuntime.dll', 'shaders/native_codec_encode.hlsl', 'lmxxf-modules'):
            with self.subTest(missing=relative):
                path = self.package / relative
                saved = self.root / 'held-update-component'
                path.rename(saved)
                try:
                    code, output = self.run_batch(stdin="yes\n")
                    self.assertEqual(code, 1, output)
                    self.assertIn('Incomplete lmxxf package', output)
                    self.assertNotIn('Uninstalling the existing', output)
                    self.assertEqual(snapshot_files(self.game), before)
                    self.assertFalse(list(self.game.glob('.amd-presr-source-*')))
                finally:
                    saved.rename(path)

    def test_setup_clean_reinstall_from_package_in_game_preserves_its_sources(self):
        self.ready_lmxxf_dual_arch()
        (self.package / "dxgi.dll").write_bytes(b"fixture proxy")
        code, output = self.run_batch(game=self.package, stdin="Y\n3\n")
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

    def test_locked_excluded_user_module_is_preserved(self):
        mods = self.ready_legacy_install()
        before = snapshot_files(self.game)
        with locked_file(mods / "user-custom.hsaco"):
            code, output = self.run_direct()
        self.assertEqual(code, 1, output)
        self.assertEqual(snapshot_files(self.game), before)
        self.assertFalse(list(self.game.glob(".lmxxf-previous-*")))

    def test_uninstall_then_install_preserves_extra_hsaco(self):
        mods = self.ready_legacy_install()
        code, output = self.run_direct(name="Uninstall")
        self.assertEqual(code, 0, output)
        self.assertEqual((mods / "user-custom.hsaco").read_bytes(), b"user-owned GPU code")
        self.assertEqual(list(mods.glob("*.hsaco")), [mods / "user-custom.hsaco"])
        self.assertTrue((mods / "user_weights.bin").exists())
        self.assert_legacy_shaders_removed(mods)
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


    def test_retained_custom_asi_enables_only_automatic_loading_after_ini_replacement(self):
        self.ready_install()
        for index, (setting, expected, plugin) in enumerate((
            ('auto', 'true', 'MFGUnlocker.asi'),
            ('false', 'false', 'MFGUnlocker.asi'),
            ('true', 'true', 'MFGUnlocker.asi'),
            ('auto', 'auto', None),
        )):
            with self.subTest(setting=setting, plugin=plugin):
                game = self.root / f'retained-asi-{index}'
                target = game / 'OptiScaler/plugins'
                target.mkdir(parents=True)
                if plugin:
                    (target / plugin).write_bytes(b'user plugin; never executed')
                (self.package / 'OptiScaler.ini').write_text(
                    '[Plugins]\nLoadAsiPlugins=' + setting + '\n', encoding='utf-8')
                (game / 'OptiScaler.ini').write_text(
                    '[Plugins]\nLoadAsiPlugins=true\nUserSentinel=old\n', encoding='utf-8')
                code, output = self.run_direct(game=game)
                self.assertEqual(code, 0, output)
                actual = (game / 'OptiScaler.ini').read_text(encoding='utf-8-sig')
                self.assertNotIn('UserSentinel', actual)
                self.assertRegex(actual, r'LoadAsiPlugins\s*=\s*' + expected)
                if plugin:
                    self.assertEqual((target / plugin).read_bytes(), b'user plugin; never executed')

    def test_custom_plugins_keep_configs_readme_and_explicit_loader_setting(self):
        self.ready_install()
        source = self.package / 'plugins'
        nested = self.package / 'OptiScaler/plugins'
        source.mkdir()
        nested.mkdir(parents=True)
        (source / 'Custom.asi').write_bytes(b'new custom plugin')
        for suffix in ('ini', 'json', 'txt'):
            (source / f'Custom.{suffix}').write_text('package default', encoding='utf-8')
        (nested / 'Nested.ini').write_text('nested package default', encoding='utf-8')
        (source / 'Fresh.ini').write_text('initial setting', encoding='utf-8')
        for directory in (source, nested):
            (directory / 'README.txt').write_text('legacy package placeholder', encoding='utf-8')
            (directory / 'OptScaler-NR-plugins-README.txt').write_text('project placeholder', encoding='utf-8')
        for index, (path, setting, expected) in enumerate((
            ('auto', None, 'true'), ('mods', 'false', 'false'),
            (str(self.root / 'shared-plugins'), 'auto', 'true'), ('auto', 'true', 'true'),
        )):
            with self.subTest(path=path, setting=setting):
                game = self.root / f'game-{index}'
                game.mkdir()
                target = (game / 'OptiScaler/plugins') if path == 'auto' else (
                    Path(path) if Path(path).is_absolute() else game / path)
                target.mkdir(parents=True)
                (target / 'README.txt').write_text('user document', encoding='utf-8')
                (target / 'Custom.asi').write_bytes(b'old custom plugin')
                configs = ['Custom.ini', 'Custom.json', 'Custom.txt', 'Nested.ini']
                for name in configs:
                    (target / name).write_text('user setting', encoding='utf-8')
                ini = '[Plugins]\nPath=' + path + '\n'
                if setting is not None:
                    ini += 'LoadAsiPlugins=' + setting + '\n'
                (self.package / 'OptiScaler.ini').write_text(ini, encoding='utf-8')
                (game / 'OptiScaler.ini').write_text(ini, encoding='utf-8')
                for _ in range(2):
                    code, output = self.run_direct(game=game)
                    self.assertEqual(code, 0, output)
                    self.assertEqual((target / 'Custom.asi').read_bytes(), b'new custom plugin')
                    self.assertEqual((target / 'README.txt').read_text(), 'user document')
                    for name in configs:
                        self.assertEqual((target / name).read_text(), 'user setting', name)
                    self.assertEqual((target / 'Fresh.ini').read_text(), 'initial setting')
                    actual = (game / 'OptiScaler.ini').read_text(encoding='utf-8-sig')
                    self.assertRegex(actual, r'LoadAsiPlugins\s*=\s*' + expected)
                code, output = self.run_direct('Uninstall', game=game)
                self.assertEqual(code, 0, output)
                self.assertEqual((target / 'README.txt').read_text(), 'user document')
                self.assertTrue((target / 'Custom.asi').exists())
                for name in configs:
                    self.assertEqual((target / name).read_text(), 'user setting')

    def test_uninstall_preserves_unowned_readme_without_install_record(self):
        for path in ('OptiScaler/plugins', 'user-mods'):
            with self.subTest(path=path):
                target = self.game / path
                target.mkdir(parents=True, exist_ok=True)
                (target / 'README.txt').write_text('user document', encoding='utf-8')
                (self.game / 'OptiScaler.ini').write_text('[Plugins]\nPath=' + path + '\n', encoding='utf-8')
                code, output = self.run_direct('Uninstall')
                self.assertEqual(code, 0, output)
                self.assertEqual((target / 'README.txt').read_text(), 'user document')


if __name__ == "__main__":
    unittest.main(verbosity=2)
