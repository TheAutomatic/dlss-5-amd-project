"""Schema 1 worker inspect + mock worker + launcher compile."""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
PS = Path(os.environ.get("SystemRoot", r"C:\Windows")) / (
    "System32/WindowsPowerShell/v1.0/powershell.exe"
)
WORKER = REPO / "tools/install/launcher-worker.ps1"
MOCK = REPO / "tools/install/launcher/mock-worker.ps1"
BUILD = REPO / "tools/build/build-launcher.ps1"


@unittest.skipUnless(os.name == "nt" and PS.exists(), "requires Windows PowerShell 5.1")
class LibraryScanTests(unittest.TestCase):
    def setUp(self):
        output = REPO / "exports/tests/library-scan"
        output.mkdir(parents=True, exist_ok=True)
        self.work = tempfile.TemporaryDirectory(prefix="case-", dir=output)
        self.addCleanup(self.work.cleanup)
        self.root = Path(self.work.name) / "Baldur's Gate 3" / "Content"
        self.root.mkdir(parents=True)

    def exe(self, relative):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"scan fixture; never executed")
        return path

    def scan(self, function="Get-OptScalerXboxExes"):
        env = dict(os.environ, OPTSCALER_SCAN_TEST_ROOT=str(self.root),
                   OPTSCALER_SCAN_TEST_SCRIPT=str(REPO / "tools/install/library-scan.ps1"))
        script = (
            "$ErrorActionPreference='Stop'; "
            ". $env:OPTSCALER_SCAN_TEST_SCRIPT; "
            f"ConvertTo-Json -InputObject @({function} $env:OPTSCALER_SCAN_TEST_ROOT) -Depth 4"
        )
        p = subprocess.run([str(PS), "-NoProfile", "-NonInteractive", "-Command", script],
                           env=env, capture_output=True, timeout=30)
        self.assertEqual(p.returncode, 0, p.stderr.decode("utf-8", "replace"))
        return json.loads(p.stdout.decode("utf-8-sig"))

    def manifest(self, entries, namespace=""):
        (self.root / "MicrosoftGame.Config").write_text(
            f'<Game {namespace}><ExecutableList>{entries}</ExecutableList></Game>', encoding="utf-8"
        )

    def test_gdk_entry_beats_helpers_and_alphabetical_guess(self):
        expected = self.exe("bin/bg3.exe")
        self.exe("aaa.exe")
        self.exe("GameLaunchHelper.exe")
        self.exe("person-model/person-worker.exe")
        (self.root / "appxmanifest.xml").write_text(
            '<Package><Applications><Application Executable="GameLaunchHelper.exe"/></Applications></Package>',
            encoding="utf-8",
        )
        self.manifest('<Executable Name="bin\\bg3.exe" Id="Game" TargetDeviceFamily="PC"/>')
        self.assertEqual([Path(c["path"]) for c in self.scan()], [expected])

    def test_namespaced_manifest_filters_non_pc_dev_missing_and_duplicate_entries(self):
        expected = self.exe("game.exe")
        self.exe("console.exe")
        self.exe("dev.exe")
        self.manifest(
            '<Executable Name="console.exe" TargetDeviceFamily="XboxOne"/>'
            '<Executable Name="dev.exe" IsDevOnly="true"/>'
            '<Executable Name="missing.exe"/>'
            '<Executable Name="game.exe"/><Executable Name="GAME.exe"/>',
            'xmlns="urn:gdk-fixture"',
        )
        self.assertEqual([Path(c["path"]) for c in self.scan()], [expected])

    def test_invalid_metadata_falls_back_without_selecting_auxiliary_exes(self):
        expected = self.exe("game.exe")
        outside = self.root.parent / "outside.exe"
        outside.write_bytes(b"outside the game")
        self.exe("GameLaunchHelper.exe")
        self.exe("person-worker.exe")
        self.exe("OptiScaler/a-worker.exe")
        self.exe("person-model/a-worker.exe")
        self.exe("_CommonRedist/a-redist.exe")
        invalid = (
            None, "<broken", '<Game><ExecutableList><Executable Name="missing.exe"/></ExecutableList></Game>',
            '<Game><ExecutableList><Executable Name="..\\outside.exe"/></ExecutableList></Game>',
            f'<Game><ExecutableList><Executable Name="{outside}"/></ExecutableList></Game>',
            '<!DOCTYPE Game [<!ENTITY target "outside.exe">]><Game><ExecutableList>'
            '<Executable Name="&target;"/></ExecutableList></Game>',
        )
        config = self.root / "MicrosoftGame.Config"
        for content in invalid:
            with self.subTest(manifest=content):
                if content is None:
                    config.unlink(missing_ok=True)
                else:
                    config.write_text(content, encoding="utf-8")
                self.assertEqual([Path(c["path"]) for c in self.scan()], [expected])

    def test_shipping_exe_not_lost_after_eight_generic_candidates(self):
        for i in range(12):
            self.exe(f"utility{i:02}.exe")
        expected = self.exe("Game/Binaries/WinGDK/Game-WinGDK-Shipping.exe")
        candidates = self.scan("Get-OptScalerPrimaryExes")
        self.assertEqual(Path(candidates[0]["path"]), expected)
        self.assertEqual(len(candidates), 8)


@unittest.skipUnless(os.name == "nt" and PS.exists(), "requires Windows PowerShell 5.1")
class LauncherWorkerTests(unittest.TestCase):
    def invoke(self, script, action, exe, decisions=None, proxy=None, package_root=None,
               backends=None, active_backend=None, language=None):
        work = Path(tempfile.mkdtemp(prefix="launcher-worker-"))
        self.addCleanup(shutil.rmtree, work)
        req = work / "request.json"
        events = work / "events.jsonl"
        result = work / "result.json"
        payload = {
            "schema": 1,
            "requestId": "test",
            "action": action,
            "gameExe": str(exe),
            "proxyChoice": proxy or "auto",
            "backendChoice": "auto",
            "explicitDecisions": decisions or {},
        }
        if package_root:
            payload["packageRoot"] = str(package_root)
        if backends is not None:
            payload["backends"] = backends
        if active_backend is not None:
            payload["activeBackend"] = active_backend
        if language is not None:
            payload["language"] = language
        req.write_text(json.dumps(payload), encoding="utf-8")
        events.write_text("", encoding="utf-8")
        cmd = [
            str(PS),
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(script),
            "-RequestJson",
            str(req),
            "-EventsJsonl",
            str(events),
            "-ResultJson",
            str(result),
        ]
        p = subprocess.run(cmd, capture_output=True, timeout=120)
        out = (p.stdout or b"").decode("utf-8", "replace")
        err = (p.stderr or b"").decode("utf-8", "replace")
        body = result.read_text(encoding="utf-8") if result.exists() else ""
        return p.returncode, json.loads(body) if body else {}, events.read_text(encoding="utf-8")

    def standalone_worker(self):
        root = Path(tempfile.mkdtemp(prefix="launcher-bundle-"))
        self.addCleanup(shutil.rmtree, root)
        pkg, game = root / "package", root / "game"
        pkg.mkdir()
        game.mkdir()
        for name in ("launcher-worker.ps1", "proxy-probe.ps1", "install-capabilities.ps1", "lmxxf-small.ps1", "lmxxf-small-manifest.json"):
            shutil.copy2(REPO / "tools/install" / name, pkg / name)
        (pkg / "OptiScaler.dll").write_bytes(b"fixture")
        (pkg / "Setup.ps1").write_text("param()\nexit 0\n", encoding="utf-8")
        exe = game / "game.exe"
        shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/whoami.exe", exe)
        return pkg, game, exe

    def test_inspect_recognizes_shipped_lmxxf_layout(self):
        pkg, game, exe = self.standalone_worker()
        (pkg / "LmxxfNrRuntime.dll").write_bytes(b"runtime")
        (pkg / "lmxxf-modules").mkdir()
        (pkg / "shaders").mkdir()
        (pkg / "shaders/native_codec_encode.hlsl").write_bytes(b"shader")
        (game / "native-game-tiled-assets").mkdir()
        (game / "native-game-tiled-assets/block0-ffn.f16").write_bytes(b"tensor")
        code, result, events = self.invoke(pkg / WORKER.name, "inspect", exe)
        self.assertEqual(code, 0, events + str(result))
        lmxxf = next(b for b in result["backends"] if b["id"] == "lmxxf")
        self.assertEqual(lmxxf["runtimeState"], "ready", lmxxf)
        self.assertFalse(lmxxf["enabled"], "A block0 marker alone must not claim a complete model")
        self.assertEqual(lmxxf["originalState"], "missing")
        self.assertEqual(lmxxf["smallState"], "missing")
        # Installed code cannot fill a missing runtime in the update package.
        (pkg / "LmxxfNrRuntime.dll").rename(game / "LmxxfNrRuntime.dll")
        code, result, events = self.invoke(pkg / WORKER.name, "inspect", exe)
        self.assertEqual(code, 0, events + str(result))
        lmxxf = next(b for b in result["backends"] if b["id"] == "lmxxf")
        self.assertEqual(lmxxf["runtimeState"], "missing", lmxxf)

    def test_inspect_recognizes_cached_daniel_runtime(self):
        pkg, game, exe = self.standalone_worker()
        (pkg / "dlssnr_on_amd_weights.bin").write_bytes(bytes(1024 * 1024))
        for runtime in (pkg / "version.dll", game / "dlssnr_amd_pass1.dll"):
            with self.subTest(runtime=runtime.name):
                runtime.write_bytes(b"runtime presence; Setup verifies its hash")
                code, result, events = self.invoke(pkg / WORKER.name, "inspect", exe)
                self.assertEqual(code, 0, events + str(result))
                daniel = next(b for b in result["backends"] if b["id"] == "daniel")
                self.assertTrue(daniel["enabled"], daniel)
                runtime.unlink()

    def test_inspect_reports_all_missing_inputs_and_the_package_folder(self):
        pkg, game, exe = self.standalone_worker()
        code, result, events = self.invoke(pkg / WORKER.name, "inspect", exe)
        self.assertEqual(code, 0, events + str(result))
        self.assertTrue(Path(result["packageRoot"]).samefile(pkg))
        backends = {b["id"]: b for b in result["backends"]}
        self.assertEqual(backends["lmxxf"]["hints"], ["lmxxf-original", "lmxxf-small"])
        self.assertEqual(set(backends["lmxxf"]["missingPackageFiles"]), {
            "LmxxfNrRuntime.dll", "lmxxf-modules/", "shaders/native_codec_encode.hlsl"})
        self.assertEqual(set(backends["daniel"]["hints"]), {
            "daniel-runtime", "daniel-weights", "daniel-setup", "daniel-source"})
        self.assertEqual(backends["mochizuki"]["hints"], ["mochi-source", "mochi-versions"])
        self.assertEqual(backends["mochizuki"]["missingPackageFiles"], ["MochizukiNrRuntime.dll"])

    def test_daniel_distinguishes_installer_from_ready_runtime(self):
        pkg, game, exe = self.standalone_worker()
        (pkg / "dlssnr_on_amd_setup.exe").write_bytes(b"fixture; never executed")
        code, result, events = self.invoke(pkg / WORKER.name, "inspect", exe)
        self.assertEqual(code, 0, events)
        daniel = next(b for b in result["backends"] if b["id"] == "daniel")
        self.assertFalse(daniel["enabled"])
        self.assertIn("daniel-runtime", daniel["hints"])
        self.assertNotIn("daniel-setup", daniel["hints"])
        (pkg / "nvngx_dlssnr.dll").write_bytes(b"source presence; Daniel's setup validates it")
        code, result, events = self.invoke(pkg / WORKER.name, "inspect", exe)
        self.assertEqual(code, 0, events)
        daniel = next(b for b in result["backends"] if b["id"] == "daniel")
        self.assertFalse(daniel["enabled"])
        self.assertEqual(daniel["runtimeState"], "preparable")
        self.assertEqual(daniel["hints"], ["daniel-preparable"])
        (pkg / "version.dll").write_bytes(b"runtime presence; Setup checks its hash")
        (pkg / "dlssnr_on_amd_weights.bin").write_bytes(bytes(1024 * 1024))
        code, result, events = self.invoke(pkg / WORKER.name, "inspect", exe)
        self.assertEqual(code, 0, events)
        daniel = next(b for b in result["backends"] if b["id"] == "daniel")
        self.assertTrue(daniel["enabled"])
        self.assertEqual(daniel["hints"], [])

    def test_mochi_reuses_game_weights_but_requires_package_assets(self):
        pkg, game, exe = self.standalone_worker()
        (pkg / "MochizukiNrRuntime.dll").write_bytes(b"runtime fixture")
        (pkg / "mochizuki-owned.txt").write_text("shaders/example.hlsl\n", encoding="utf-8")
        for name in ("dlssnr-amd/shaders/example.hlsl", "mochizuki-int4.ps1",
                     "model-tools/dlssnr-int4-weights.exe", "model-tools/dlssnr-int4.sha256"):
            path = pkg / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"package asset")
        model = game / "dlssnr-amd/dlssnr-small.bin"
        model.parent.mkdir()
        model.write_bytes(b"NRMODEL1" + (255).to_bytes(4, "little") + bytes(20))
        code, result, events = self.invoke(pkg / WORKER.name, "inspect", exe)
        self.assertEqual(code, 0, events)
        mochi = next(b for b in result["backends"] if b["id"] == "mochizuki")
        self.assertTrue(mochi["enabled"], mochi)
        self.assertEqual(mochi["hints"], [])
        self.assertEqual(mochi["missingPackageFiles"], [])
        (pkg / "dlssnr-amd/shaders/example.hlsl").unlink()
        code, result, events = self.invoke(pkg / WORKER.name, "inspect", exe)
        self.assertEqual(code, 0, events)
        mochi = next(b for b in result["backends"] if b["id"] == "mochizuki")
        self.assertFalse(mochi["enabled"])
        self.assertEqual(mochi["missingPackageFiles"], ["dlssnr-amd/shaders/example.hlsl"])
        self.assertEqual(mochi["hints"], [])

    def test_mochi_source_must_match_an_accepted_hash(self):
        pkg, game, exe = self.standalone_worker()
        (pkg / "nvngx_dlssnr.dll").write_bytes(b"unsupported source")
        code, result, events = self.invoke(pkg / WORKER.name, "inspect", exe)
        self.assertEqual(code, 0, events)
        mochi = next(b for b in result["backends"] if b["id"] == "mochizuki")
        self.assertEqual(mochi["hints"], ["mochi-unsupported-source", "mochi-versions"])
        self.assertEqual(mochi["modelState"], "missing")
        # Substitute a trusted source only in the isolated capability fixture.
        source = b"trusted extraction source fixture; never loaded"
        (game / "nvngx_dlssnr.dll").write_bytes(source)
        capability = pkg / "install-capabilities.ps1"
        text = capability.read_text(encoding="utf-8-sig").replace(
            "2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f",
            hashlib.sha256(source).hexdigest())
        capability.write_text(text, encoding="utf-8-sig")
        code, result, events = self.invoke(pkg / WORKER.name, "inspect", exe)
        self.assertEqual(code, 0, events)
        mochi = next(b for b in result["backends"] if b["id"] == "mochizuki")
        self.assertEqual(mochi["modelState"], "preparable")
        self.assertEqual(mochi["hints"], ["mochi-preparable"])

    def test_mochi_reports_per_model_preparable_states(self):
        pkg, game, exe = self.standalone_worker()
        (pkg / "MochizukiNrRuntime.dll").write_bytes(b"runtime fixture")
        (pkg / "mochizuki-owned.txt").write_text("shaders/example.hlsl\n", encoding="utf-8")
        original = game / "dlssnr-amd/dlssnr.bin"
        original.parent.mkdir()
        original.write_bytes(b"NRMODEL1" + (599).to_bytes(4, "little") + bytes(20))

        def patch_source_hash(content, old_hash):
            source = pkg / "nvngx_dlssnr.dll"
            source.write_bytes(content)
            capability = pkg / "install-capabilities.ps1"
            text = capability.read_text(encoding="utf-8-sig").replace(
                old_hash, hashlib.sha256(content).hexdigest())
            capability.write_text(text, encoding="utf-8-sig")
            return self.invoke(pkg / WORKER.name, "inspect", exe)

        # Original already extracted; a 310.9.16 source can still prepare small -> the launcher's
        # Small option must stay enabled (ready + preparable mix).
        code, result, events = patch_source_hash(
            b"trusted 310.9.16 source fixture; never loaded",
            "2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f")
        self.assertEqual(code, 0, events)
        mochi = next(b for b in result["backends"] if b["id"] == "mochizuki")
        self.assertEqual(mochi["originalState"], "ready", mochi)
        self.assertEqual(mochi["smallState"], "preparable", mochi)

        # A 310.8.0 source prepares neither small nor (with original gone) anything beyond original.
        original.unlink()
        code, result, events = patch_source_hash(
            b"trusted 310.8.0 source fixture; never loaded",
            "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e")
        self.assertEqual(code, 0, events)
        mochi = next(b for b in result["backends"] if b["id"] == "mochizuki")
        self.assertEqual(mochi["originalState"], "preparable", mochi)
        self.assertEqual(mochi["smallState"], "missing", mochi)

    def test_mochi_small_follows_new_model_source_across_candidates(self):
        # Setup prefers a 310.9.16 source wherever it sits; the capability scan must not stop at the
        # first accepted candidate (pkg 310.8.0) and lose small preparability from the game copy.
        pkg, game, exe = self.standalone_worker()
        (pkg / "MochizukiNrRuntime.dll").write_bytes(b"runtime fixture")
        (pkg / "mochizuki-owned.txt").write_text("shaders/example.hlsl\n", encoding="utf-8")
        (pkg / "nvngx_dlssnr.dll").write_bytes(b"trusted 310.8.0 source fixture; never loaded")
        (game / "nvngx_dlssnr.dll").write_bytes(b"trusted 310.9.16 source fixture; never loaded")
        capability = pkg / "install-capabilities.ps1"
        text = capability.read_text(encoding="utf-8-sig")
        text = text.replace("2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f",
                            hashlib.sha256(b"trusted 310.9.16 source fixture; never loaded").hexdigest())
        text = text.replace("e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e",
                            hashlib.sha256(b"trusted 310.8.0 source fixture; never loaded").hexdigest())
        capability.write_text(text, encoding="utf-8-sig")
        code, result, events = self.invoke(pkg / WORKER.name, "inspect", exe)
        self.assertEqual(code, 0, events)
        mochi = next(b for b in result["backends"] if b["id"] == "mochizuki")
        self.assertEqual(mochi["originalState"], "preparable", mochi)
        self.assertEqual(mochi["smallState"], "preparable", mochi)

    def test_mochi_installed_model_does_not_authorize_untrusted_source(self):
        # Small already extracted; an unsupported nvngx_dlssnr.dll must NOT make original preparable
        # (the earlier hash filter is skipped when a model is installed).
        pkg, game, exe = self.standalone_worker()
        (pkg / "MochizukiNrRuntime.dll").write_bytes(b"runtime fixture")
        (pkg / "mochizuki-owned.txt").write_text("shaders/example.hlsl\n", encoding="utf-8")
        small = game / "dlssnr-amd/dlssnr-small.bin"
        small.parent.mkdir()
        small.write_bytes(b"NRMODEL1" + (255).to_bytes(4, "little") + bytes(20))
        (pkg / "nvngx_dlssnr.dll").write_bytes(b"unsupported source; never hashed")
        code, result, events = self.invoke(pkg / WORKER.name, "inspect", exe)
        self.assertEqual(code, 0, events)
        mochi = next(b for b in result["backends"] if b["id"] == "mochizuki")
        self.assertEqual(mochi["smallState"], "ready", mochi)
        self.assertEqual(mochi["originalState"], "missing", mochi)

    def test_requested_package_selects_its_own_scripts(self):
        pkg, game, exe = self.standalone_worker()
        update = pkg.parent / "requested update"
        update.mkdir()
        (update / "OptiScaler.dll").write_bytes(b"new package")
        stub = "param([string]$GameDir)\n[IO.File]::WriteAllText((Join-Path $GameDir 'used-script.txt'), $PSCommandPath)\nexit 0\n"
        (pkg / "Uninstall_OptiScaler_NR.ps1").write_text("exit 0\n", encoding="utf-8")
        for script_name, action in (("Setup.ps1", "install"), ("Uninstall_OptiScaler_NR.ps1", "uninstall")):
            with self.subTest(action=action):
                (update / script_name).write_text(stub, encoding="utf-8")
                code, result, events = self.invoke(
                    pkg / WORKER.name, action, exe, proxy="dxgi.dll", package_root=update
                )
                self.assertEqual(code, 0, events + str(result))
                marker = game / "used-script.txt"
                self.assertTrue(marker.exists(), events)
                self.assertTrue(Path(marker.read_text()).samefile(update / script_name))
                marker.unlink()

    def test_empty_or_invalid_backend_selection_stops_before_setup(self):
        pkg, game, exe = self.standalone_worker()
        (pkg / "Setup.ps1").write_text(
            "param([string]$GameDir)\n[IO.File]::WriteAllText((Join-Path $GameDir 'should-not-run'), 'bad')\nexit 0\n",
            encoding="utf-8",
        )
        for backends, active in (([], None), (["unknown"], None), (["daniel", "daniel"], None),
                                 (["daniel"], "lmxxf"), (["daniel"], "unknown")):
            with self.subTest(backends=backends, active=active):
                code, result, events = self.invoke(
                    pkg / WORKER.name, "install", exe, proxy="dxgi.dll",
                    backends=backends, active_backend=active
                )
                self.assertEqual(code, 1, events + str(result))
                self.assertEqual(result["status"], "failed", result)
                self.assertFalse((game / "should-not-run").exists())

    def test_invalid_requested_package_does_not_fall_back_to_adjacent_package(self):
        pkg, game, exe = self.standalone_worker()
        stub = "param([string]$GameDir)\n[IO.File]::WriteAllText((Join-Path $GameDir 'should-not-run'), 'bad')\nexit 0\n"
        for name in ("Setup.ps1", "Uninstall_OptiScaler_NR.ps1"):
            (pkg / name).write_text(stub, encoding="utf-8")
        for action in ("install", "uninstall"):
            with self.subTest(action=action):
                code, result, events = self.invoke(
                    pkg / WORKER.name, action, exe, proxy="dxgi.dll", package_root=pkg / "missing"
                )
                self.assertEqual(code, 1, events + str(result))
                self.assertEqual(result["status"], "failed", result)
                self.assertFalse((game / "should-not-run").exists())

    def test_inspect_whoami_copy(self):
        d = Path(tempfile.mkdtemp(prefix="launcher-inspect-"))
        exe = d / "whoami.exe"
        shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/whoami.exe", exe)
        code, result, events = self.invoke(WORKER, "inspect", exe)
        self.assertEqual(code, 0, events)
        self.assertEqual(result["status"], "success")
        self.assertEqual(result["schema"], 1)
        self.assertTrue(Path(result["effectiveGameDir"]).samefile(d))
        self.assertIn("version.dll", [c["name"] for c in result["candidates"]])
        self.assertNotIn("dinput8.dll", [c["name"] for c in result["candidates"]])

    def test_rejects_relative_path(self):
        work = Path(tempfile.mkdtemp(prefix="launcher-rel-"))
        req = work / "request.json"
        events = work / "events.jsonl"
        result = work / "result.json"
        req.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "requestId": "x",
                    "action": "inspect",
                    "gameExe": "whoami.exe",
                    "proxyChoice": "auto",
                    "backendChoice": "auto",
                }
            ),
            encoding="utf-8",
        )
        events.write_text("", encoding="utf-8")
        p = subprocess.run(
            [
                str(PS),
                "-NoProfile",
                "-NonInteractive",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(WORKER),
                "-RequestJson",
                str(req),
                "-EventsJsonl",
                str(events),
                "-ResultJson",
                str(result),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(p.returncode, 0)

    def test_mock_worker(self):
        d = Path(tempfile.mkdtemp(prefix="launcher-mock-"))
        exe = d / "game.exe"
        exe.write_bytes(b"MZ")
        code, result, events = self.invoke(MOCK, "inspect", exe)
        self.assertEqual(code, 0, events)
        self.assertEqual(result["resolvedProxy"], "dxgi.dll")

    def test_windowsapps_name_maps_to_xboxgames(self):
        xg = Path(r"C:\XboxGames")
        hits = list(xg.glob("**/SandFall-WinGDK-Shipping.exe")) if xg.is_dir() else []
        if not hits:
            self.skipTest("XboxGames shipping exe not present")
        fake = Path(r"C:\Program Files\WindowsApps\DummyPkg\SandFall-WinGDK-Shipping.exe")
        code, result, events = self.invoke(WORKER, "inspect", fake)
        self.assertEqual(code, 0, events + str(result))
        folder = Path(result["effectiveGameDir"])
        self.assertTrue("XboxGames" in folder.parts)
        self.assertEqual(folder, hits[0].parent)

    def test_inspect_path_with_spaces(self):
        d = Path(tempfile.mkdtemp(prefix="launcher space-"))
        folder = d / "Clair Obscur- Expedition 33"
        folder.mkdir()
        exe = folder / "game.exe"
        shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/whoami.exe", exe)
        code, result, events = self.invoke(WORKER, "inspect", exe)
        self.assertEqual(code, 0, events)
        self.assertTrue(Path(result["effectiveGameDir"]).samefile(folder))

    def test_foreign_proxy_key_value_array_is_accepted(self):
        d = Path(tempfile.mkdtemp(prefix="launcher-kv-"))
        pkg = Path(tempfile.mkdtemp(prefix="launcher-pkg-"))
        (pkg / "OptiScaler.dll").write_bytes(b"fixture")
        (pkg / "Setup.ps1").write_text("# dummy", encoding="utf-8")
        exe = d / "game.exe"
        shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/whoami.exe", exe)
        (d / "dxgi.dll").write_bytes(b"not-optiscaler")
        code, result, events = self.invoke(
            WORKER,
            "install",
            exe,
            {"foreignProxy": [{"Key": "dxgi.dll", "Value": "backup"}]},
            proxy="dxgi.dll",
            package_root=pkg,
        )
        self.assertNotEqual(code, 2, events + str(result))
        ids = [x.get("id") for x in result.get("requiredDecisions") or []]
        self.assertNotIn("foreignProxy", ids)

    def test_foreign_proxy_needs_input(self):
        d = Path(tempfile.mkdtemp(prefix="launcher-foreign-"))
        pkg = Path(tempfile.mkdtemp(prefix="launcher-pkg-"))
        (pkg / "OptiScaler.dll").write_bytes(b"fixture")
        (pkg / "Setup.ps1").write_text("# dummy", encoding="utf-8")
        exe = d / "game.exe"
        shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/whoami.exe", exe)
        (d / "dxgi.dll").write_bytes(b"not-optiscaler")
        code, result, events = self.invoke(WORKER, "install", exe, proxy="dxgi.dll", package_root=pkg)
        self.assertEqual(code, 2, events)
        self.assertEqual(result["status"], "needsInput")
        ids = [x["id"] for x in result.get("requiredDecisions") or []]
        self.assertIn("foreignProxy", ids)

    def test_mochizuki_install_is_informational_and_never_moved(self):
        d = Path(tempfile.mkdtemp(prefix="launcher-mochi-"))
        pkg = Path(tempfile.mkdtemp(prefix="launcher-pkg-"))
        self.addCleanup(shutil.rmtree, d, True)
        self.addCleanup(shutil.rmtree, pkg, True)
        (pkg / "OptiScaler.dll").write_bytes(b"fixture")
        (pkg / "Setup.ps1").write_text("# dummy", encoding="utf-8")
        exe = d / "game.exe"
        shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/whoami.exe", exe)
        theirs = ("dxgi.dll", "dlssnr_core.dll", "nvngx_dlssnr.dll")
        for name in theirs:
            (d / name).write_bytes(b"mochizuki")
        (d / "dlssnr-amd").mkdir()
        (d / "dlssnr-amd-install.txt").write_text(
            "F dxgi.dll\nF dlssnr_core.dll\nF nvngx_dlssnr.dll\nD dlssnr-amd\nF ..\\outside.dll\n", encoding="utf-8")

        code, result, events = self.invoke(WORKER, "inspect", exe, package_root=pkg)
        self.assertEqual(code, 0, events)
        found = result["mochizukiInstall"]
        self.assertTrue(found["recorded"])
        self.assertEqual(found["proxy"], "dxgi.dll")
        self.assertEqual(sorted(found["files"]), sorted(("dxgi.dll", "dlssnr_core.dll", "dlssnr-amd-install.txt")))

        # No mochizukiInstall decision exists. The only question is the ordinary
        # foreignProxy one for his dxgi.dll at our chosen entry; his loadable and
        # inert files alike are never moved by the installer.
        code, result, events = self.invoke(WORKER, "install", exe, proxy="dxgi.dll", package_root=pkg)
        self.assertEqual(code, 2, events)
        ids = [x["id"] for x in result["requiredDecisions"]]
        self.assertIn("foreignProxy", ids)
        self.assertNotIn("mochizukiInstall", ids)
        code, result, events = self.invoke(WORKER, "install", exe,
                                           {"foreignProxy": {"dxgi.dll": "backup"}},
                                           proxy="dxgi.dll", package_root=pkg)
        self.assertNotEqual(result["status"], "needsInput", events)
        self.assertTrue((d / "dlssnr_core.dll").exists(), events)
        self.assertTrue((d / "nvngx_dlssnr.dll").exists(), events)
        self.assertFalse((d / "OptiScaler-mochizuki-backup").exists(), events)

    def test_mochizuki_own_install_and_inert_leftovers_are_not_reported(self):
        d = Path(tempfile.mkdtemp(prefix="launcher-mochi-self-"))
        pkg = Path(tempfile.mkdtemp(prefix="launcher-pkg-"))
        self.addCleanup(shutil.rmtree, d, True)
        self.addCleanup(shutil.rmtree, pkg, True)
        (pkg / "OptiScaler.dll").write_bytes(b"fixture")
        (pkg / "Setup.ps1").write_text("# dummy", encoding="utf-8")
        exe = d / "game.exe"
        shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/whoami.exe", exe)

        # This project's own installation: its proxy at a candidate name, its models in
        # dlssnr-amd\ and its own ownership record. It must never be reported as a
        # Mochizuki installation -- a reinstall or update must not prompt about itself.
        (d / "dxgi.dll").write_bytes(b"ours")
        (d / "mochizuki-owned.txt").write_text("shaders/example.hlsl\n", encoding="utf-8")
        (d / "dlssnr-amd").mkdir()
        (d / "dlssnr-amd" / "dlssnr.bin").write_bytes(b"model")
        code, result, events = self.invoke(WORKER, "inspect", exe, package_root=pkg)
        self.assertEqual(code, 0, events)
        self.assertFalse(result.get("mochizukiInstall"), events)

        # His inert leftovers beside ours: the nvngx pair, LICENSE, ini and his int4 model
        # (which the installer reuses) are not loadable and not ours to move. Even his
        # record naming a marker does not report anything while no loadable file exists.
        for name in ("nvngx_dlssnr.dll", "nvngx.dll_dlssnr.dll", "LICENSE"):
            (d / name).write_bytes(b"mochizuki")
        (d / "dlssnr-amd" / "dlssnr-int4.bin").write_bytes(b"his-model")
        (d / "dlssnr-amd-install.txt").write_text(
            "F nvngx_dlssnr.dll\nF nvngx.dll_dlssnr.dll\nF LICENSE\nF OptiScaler.ini\n"
            "F dlssnr-amd\\dlssnr-int4.bin\nF dlssnr_amd.addon64\nD dlssnr-amd\n", encoding="utf-8")
        code, result, events = self.invoke(WORKER, "inspect", exe, package_root=pkg)
        self.assertEqual(code, 0, events)
        self.assertFalse(result.get("mochizukiInstall"), events)

        # A loadable marker on disk is the conflict, record or not.
        (d / "dlssnr_core.dll").write_bytes(b"mochizuki")
        code, result, events = self.invoke(WORKER, "inspect", exe, package_root=pkg)
        self.assertEqual(code, 0, events)
        found = result["mochizukiInstall"]
        self.assertTrue(found["detected"])
        self.assertTrue(found["recorded"])
        self.assertEqual(sorted(found["files"]), ["dlssnr-amd-install.txt", "dlssnr_core.dll"])
        self.assertEqual(found["proxy"], "")

    def test_install_update_uninstall_isolated(self):
        pkg = Path(tempfile.mkdtemp(prefix="launcher-pkg-"))
        game = Path(tempfile.mkdtemp(prefix="launcher-game-"))
        shutil.copy2(REPO / "tools/install-amd-presr.ps1", pkg / "Setup.ps1")
        shutil.copy2(REPO / "tools/uninstall-amd-presr.ps1", pkg / "Uninstall_OptiScaler_NR.ps1")
        shutil.copy2(REPO / "tools/lmxxf-module-package.ps1", pkg / "lmxxf-module-package.ps1")
        shutil.copy2(REPO / "tools/install/launcher-worker.ps1", pkg / "launcher-worker.ps1")
        shutil.copy2(REPO / "tools/install/proxy-probe.ps1", pkg / "proxy-probe.ps1")
        shutil.copy2(REPO / "tools/install/install-capabilities.ps1", pkg / "install-capabilities.ps1")
        shutil.copy2(REPO / "VERSION", pkg / "VERSION")
        fake = b"Harmless installer test data; not an executable."
        digest = hashlib.sha256(fake).hexdigest().upper()
        text, n = re.subn(
            r"(\$expectedA030\s*=\s*)'[A-F0-9]{64}'",
            r"\g<1>'" + digest + "'",
            (pkg / "Setup.ps1").read_text(encoding="utf-8-sig"),
        )
        self.assertEqual(n, 1)
        (pkg / "Setup.ps1").write_bytes(
            b"\xef\xbb\xbf" + text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")
        )
        (pkg / "OptiScaler.dll").write_bytes(b"fixture proxy")
        (pkg / "version.dll").write_bytes(fake)
        (pkg / "dlssnr_on_amd_weights.bin").write_bytes(bytes(1024 * 1024))
        (pkg / "OptiScaler.ini").write_text("[DlssNr]\nEnabled=true\n", encoding="utf-8")
        exe = game / "game.exe"
        shutil.copy2(Path(os.environ["SystemRoot"]) / "System32/whoami.exe", exe)
        (game / "plugins").mkdir()
        (game / "plugins" / "user.asi").write_bytes(b"plugin")
        (game / "README.txt").write_text("keep me", encoding="utf-8")
        worker = pkg / "launcher-worker.ps1"

        def go(action, language="en"):
            return self.invoke(
                worker,
                action,
                exe,
                {"existingIni": "keep", "foreignProxy": {"dxgi.dll": "backup"}},
                proxy="dxgi.dll",
                backends=["daniel"],
                active_backend="daniel",
                language=language,
            )

        code, result, events = go("install", language="zh")
        self.assertEqual(code, 0, events + str(result))
        self.assertIn("本软件免费", events)
        self.assertNotIn("This software is free.", events)
        self.assertTrue((game / "dxgi.dll").exists() or (game / "amd-presr-install.txt").exists(), result)
        code, result, events = go("update")
        self.assertEqual(code, 0, events + str(result))
        self.assertNotIn("本软件免费", events)
        self.assertIn("This software is free.", events)
        self.assertRegex((game / "OptiScaler.ini").read_text(encoding="utf-8-sig"), r"NrBackend\s*=\s*daniel")
        code, result, events = go("uninstall")
        self.assertEqual(code, 0, events + str(result))
        self.assertTrue((game / "plugins" / "user.asi").exists())
        self.assertTrue((game / "README.txt").exists())

    def test_build_launcher_exe(self):
        out = REPO / "exports/launcher"
        p = subprocess.run(
            [
                str(PS),
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(BUILD),
                "-OutDir",
                str(out),
            ],
            capture_output=True,
        )
        text = (p.stdout or b"").decode("utf-8", "replace") + (p.stderr or b"").decode(
            "utf-8", "replace"
        )
        self.assertEqual(p.returncode, 0, text)
        exe = out / "OptScalerNR.Launcher.exe"
        self.assertTrue(exe.is_file())
        self.assertGreater(exe.stat().st_size, 4096)
        stamp = (out / "build.json").read_text(encoding="utf-8")
        self.assertIn("exe", stamp)
