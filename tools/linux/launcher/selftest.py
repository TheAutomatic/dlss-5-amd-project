#!/usr/bin/env python3
"""Self-test for the OptiScaler NR Linux launcher (standard library only).

    python3 launcher/selftest.py                      unit tests; plus the end-to-end test when this
                                                      file sits inside an extracted package
    python3 launcher/selftest.py --package DIR [...]  end-to-end test against these package folders
    python3 launcher/selftest.py --unit-only          skip the end-to-end test

The end-to-end test never touches the given package: it hard-link-copies it into a temporary folder
and only there creates a dummy model and points the copied install.sh's two SHA-256 constants at it
(the real NVIDIA model cannot be shipped or downloaded). It then drives the launcher's HTTP API:
install -> status -> reinstall -> remove for every route of the package, on fake 64-bit and 32-bit
game folders, and checks that the game folders return exactly to their original file set.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from launcher import desktop, jobs, library  # noqa: E402
from launcher.package import Package, find_package, is_package_root  # noqa: E402
from launcher.server import App, make_server  # noqa: E402
from launcher.state import State  # noqa: E402

PACKAGES: list = []
LAUNCH_DXGI = 'WINEDLLOVERRIDES="dxgi=n,b" %command%'
LAUNCH_VK = 'WINEDLLOVERRIDES="winevulkan=n,b;vulkan-1=n,b" %command%'


# ------------------------------------------------------------------------------------------------
# helpers
# ------------------------------------------------------------------------------------------------
def make_pe(path: Path, bits: int, size: int = 4096) -> Path:
    """A tiny file with just enough of a PE header for bitness detection."""
    path.parent.mkdir(parents=True, exist_ok=True)
    head = bytearray(0x80)
    head[0:2] = b"MZ"
    struct.pack_into("<I", head, 0x3C, 0x80)
    pe = b"PE\0\0" + struct.pack("<H", 0x8664 if bits == 64 else 0x14C) + b"\0" * 18
    path.write_bytes(bytes(head) + pe + b"\0" * max(0, size - len(head) - len(pe)))
    return path


def write(path: Path, text: str = "x") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def snapshot(folder: Path) -> list:
    out = []
    for root, dirs, files in os.walk(folder):
        for d in dirs:
            out.append((os.path.relpath(os.path.join(root, d), folder) + "/", None))
        for f in files:
            p = os.path.join(root, f)
            out.append((os.path.relpath(p, folder), hashlib.sha256(Path(p).read_bytes()).hexdigest()))
    return sorted(out)


class Client:
    """Minimal HTTP client for the launcher API."""

    def __init__(self, app: App):
        self.app = app
        self.base = "http://127.0.0.1:%d" % app.port

    def call(self, path, body=None, token=True, headers=None, raw=False):
        h = {"Host": "127.0.0.1:%d" % self.app.port}
        if token:
            h["X-NR-Token"] = self.app.token if token is True else token
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            h["Content-Type"] = "application/json"
        h.update(headers or {})
        req = urllib.request.Request(self.base + path, data=data, headers=h, method="POST" if body is not None else "GET")
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                payload = r.read()
                return r.status, (payload if raw else json.loads(payload or b"null"))
        except urllib.error.HTTPError as e:
            payload = e.read()
            try:
                return e.code, json.loads(payload)
            except ValueError:
                return e.code, payload

    def wait_job(self, job_id, timeout=600):
        deadline = time.time() + timeout
        while time.time() < deadline:
            status, r = self.call("/api/job?id=%d" % job_id)
            assert status == 200, r
            if r["job"]["state"] != "running":
                return r["job"]
            time.sleep(0.2)
        raise AssertionError("job %s timed out" % job_id)


class ServerFixture:
    def __init__(self, pkg_root, home: Path, state_path: Path):
        self.app = App(Package(pkg_root), State(state_path), home=home, wait_for_first_contact=True)
        self.httpd = make_server(self.app, 0)
        self.thread = threading.Thread(target=self.httpd.serve_forever, kwargs={"poll_interval": 0.1}, daemon=True)
        self.thread.start()
        self.client = Client(self.app)

    def close(self):
        self.app.quit.set()
        self.httpd.shutdown()
        self.httpd.server_close()


# ------------------------------------------------------------------------------------------------
# unit tests
# ------------------------------------------------------------------------------------------------
class TestVdf(unittest.TestCase):
    def test_libraryfolders_new_format(self):
        text = '''// comment
"libraryfolders"
{
\t"0"
\t{
\t\t"path"\t\t"/home/deck/.local/share/Steam"
\t\t"label"\t\t""
\t\t"apps" { "620" "123" "1091500" "456" }
\t}
\t"1" { "path" "/run/media/deck/SD card/SteamLibrary" }
}'''
        d = library.parse_vdf(text)
        self.assertEqual(d["libraryfolders"]["0"]["path"], "/home/deck/.local/share/Steam")
        self.assertEqual(d["libraryfolders"]["1"]["path"], "/run/media/deck/SD card/SteamLibrary")
        self.assertEqual(d["libraryfolders"]["0"]["apps"]["1091500"], "456")

    def test_old_format_escapes_and_case(self):
        d = library.parse_vdf('"LibraryFolders" { "TimeNextStatsReport" "1" "1" "D:\\\\Games\\\\Steam" "Name" "a \\"q\\" b" }')
        lf = d["libraryfolders"]
        self.assertEqual(lf["1"], "D:\\Games\\Steam")
        self.assertEqual(lf["name"], 'a "q" b')

    def test_appmanifest(self):
        d = library.parse_vdf('"AppState"\n{\n"appid" "1091500"\n"Name" "Cyberpunk 2077"\n"installdir" "Cyberpunk 2077"\n"StateFlags" "4"\n}')
        self.assertEqual(d["appstate"]["name"], "Cyberpunk 2077")
        self.assertEqual(d["appstate"]["installdir"], "Cyberpunk 2077")

    def test_garbage_does_not_raise(self):
        self.assertIsInstance(library.parse_vdf('"a" { "b" { "c" '), dict)
        self.assertEqual(library.parse_vdf(""), {})


class TestPe(unittest.TestCase):
    def test_bits(self):
        with tempfile.TemporaryDirectory() as td:
            t = Path(td)
            self.assertEqual(library.pe_bits(make_pe(t / "a64.exe", 64)), 64)
            self.assertEqual(library.pe_bits(make_pe(t / "a32.exe", 32)), 32)
            self.assertIsNone(library.pe_bits(write(t / "text.exe", "not a pe")))
            (t / "trunc.exe").write_bytes(b"MZ" + b"\0" * 10)
            self.assertIsNone(library.pe_bits(t / "trunc.exe"))
            far = bytearray(0x2000)  # PE header beyond the first 4 KiB
            far[0:2] = b"MZ"
            struct.pack_into("<I", far, 0x3C, 0x1800)
            far[0x1800:0x1806] = b"PE\0\0" + struct.pack("<H", 0x14C)
            (t / "far.exe").write_bytes(bytes(far))
            self.assertEqual(library.pe_bits(t / "far.exe"), 32)
            self.assertIsNone(library.pe_bits(t / "missing.exe"))
            self.assertEqual(library.folder_bits(t), [32, 64])


class TestExeDiscovery(unittest.TestCase):
    def test_unreal_and_largest(self):
        with tempfile.TemporaryDirectory() as td:
            t = Path(td)
            ue = t / "ue"
            make_pe(ue / "Launcher.exe", 64, 900000)
            make_pe(ue / "Proj" / "Binaries" / "Win64" / "Proj-Win64-Shipping.exe", 64, 5000)
            make_pe(ue / "Engine" / "Binaries" / "Win64" / "CrashReportClient.exe", 64, 9000000)
            self.assertEqual(library.find_game_exe(ue).name, "Proj-Win64-Shipping.exe")
            folder, exe = library.resolve_game_folder(ue)
            self.assertEqual(folder, ue / "Proj" / "Binaries" / "Win64")

            g = t / "plain"
            make_pe(g / "bin" / "x64" / "Game.exe", 64, 50000)
            make_pe(g / "_CommonRedist" / "vcredist" / "vc_redist.x64.exe", 64, 9000000)
            make_pe(g / "UnityCrashHandler64.exe", 64, 900000)
            make_pe(g / "unins000.exe", 32, 800000)
            self.assertEqual(library.find_game_exe(g), g / "bin" / "x64" / "Game.exe")
            folder, exe = library.resolve_game_folder(g)  # only helper exes directly in it -> the game below
            self.assertEqual(folder, g / "bin" / "x64")
            make_pe(g / "Direct.exe", 64, 10)
            self.assertEqual(library.resolve_game_folder(g)[0], g)  # a real exe in the picked folder wins

            only_helpers = t / "helpers"
            make_pe(only_helpers / "GameLauncher.exe", 64, 100)
            self.assertEqual(library.find_game_exe(only_helpers).name, "GameLauncher.exe")


class TestDetection(unittest.TestCase):
    def setUp(self):
        self.td = tempfile.TemporaryDirectory()
        self.t = Path(self.td.name)

    def tearDown(self):
        self.td.cleanup()

    def mk(self, name, manifest, files, preset=None, ini=None):
        d = self.t / name
        for f in files:
            if f.endswith("/"):
                (d / f).mkdir(parents=True, exist_ok=True)
            else:
                write(d / f)
        if manifest is not None:
            write(d / library.MANIFEST, "\n".join(manifest) + "\n")
        if preset:
            write(d / "ReShadePreset.ini", preset)
        if ini:
            write(d / "dlssnr-amd.ini", ini)
        return d

    def test_routes(self):
        ini = "[Network]\n; [RenoDX] in a comment\nModel = CC_SILVER_AARDWOLD\n[RenoDX]\n; see [Network]\nEnabled = 0\n"
        d = self.mk("opti", ["dlssnr-amd-install.txt", "dlssnr-amd/", "dxgi.dll", "OptiScaler.ini", "OptiScaler/"],
                    ["dlssnr-amd/", "dxgi.dll", "OptiScaler.ini", "OptiScaler/"], ini=ini)
        s = library.detect_install(d)
        self.assertEqual((s["installed"], s["route"], s["model"], s["style"]), (True, "optiscaler", "CC_SILVER_AARDWOLD", "wilsjo2"))
        d = self.mk("rs", ["dlssnr-amd-install.txt", "dxgi.dll", "ReShade.ini", "ReShadePreset.ini"],
                    ["dxgi.dll", "ReShade.ini"], ini="[RenoDX]\nEnabled = 1\n")
        s = library.detect_install(d)
        self.assertEqual((s["route"], s["style"]), ("reshade-feeder", "renodx"))
        d = self.mk("vk", ["dlssnr-amd-install.txt", "vulkan-1.dll", "winevulkan.dll", "ReShadePreset.ini"],
                    ["vulkan-1.dll", "winevulkan.dll"], preset="PreprocessorDefinitions=DLSS5_MV_PROVIDER=2\n")
        self.assertEqual(library.detect_install(d)["route"], "reshade-feeder-vulkan")
        d = self.mk("dx9", ["dlssnr-amd-install.txt", "vulkan-1.dll", "winevulkan.dll", "ReShadePreset.ini"],
                    ["vulkan-1.dll", "winevulkan.dll"], preset="PreprocessorDefinitions=X=1,RESHADE_DEPTH_INPUT_IS_REVERSED=0\n")
        self.assertEqual(library.detect_install(d)["route"], "reshade-feeder-dx9")
        d = self.mk("foreign", None, ["dxgi.dll", "Game.exe"])
        s = library.detect_install(d)
        self.assertEqual((s["installed"], s["foreign"]), (False, "dxgi.dll"))
        d = self.mk("clean", None, ["Game.exe"])
        self.assertEqual(library.detect_install(d), {"installed": False, "route": None, "entries": 0, "foreign": None,
                                                     "model": None, "style": None})


class TestOutputParsing(unittest.TestCase):
    def test_install_output(self):
        lines = [("out", "Making CC_SILVER_AARDWOLD_OPTIMIZED's weights ..."),
                 ("err", "Note: this is the 64-bit package but the game exe is 32-bit; DLLs of the other bitness will not load."),
                 ("out", ""), ("out", "Installed the reshade-feeder route into: /g (RenoDX style)"),
                 ("out", 'Steam launch options:  WINEDLLOVERRIDES="dxgi=n,b" %command%'),
                 ("out", "For CC_SILVER_AARDWOLD_OPTIMIZED with its int4 layers in game, use this line instead (every network can be switched to):"),
                 ("out", '                 VK_ADD_LAYER_PATH="/g/dlssnr-amd/int4/layer" VK_INSTANCE_LAYERS=VK_LAYER_DLSSNR_int4 WINEDLLOVERRIDES="dxgi=n,b" %command%'),
                 ("out", "Uninstall:  bash install.sh \"/g\" remove")]
        r = jobs.parse_output(lines)
        self.assertEqual(r["launch"], LAUNCH_DXGI)
        self.assertTrue(r["launchAlt"].startswith('VK_ADD_LAYER_PATH="/g/dlssnr-amd/int4/layer"'))
        self.assertTrue(r["launchAlt"].endswith("%command%"))
        self.assertEqual(r["installed"], "reshade-feeder")
        self.assertIsNone(r["error"])
        self.assertEqual(len(r["notes"]), 1)

    def test_errors_logs_remove(self):
        r = jobs.parse_output([("err", "The package has no model file: point --dll at nvngx_dlssnr.dll (310.9.16 - both models - or 310.8.0) or its zip."),
                               ("err", "Only the first time: the extracted model is kept in this package's dlssnr-amd/ and used by later installs.")])
        self.assertIn("no model file", r["error"])
        self.assertEqual(jobs.parse_output([("out", "Logs packed: /home/u/dlssnr-amd-logs-1.tar.gz")])["logs"], "/home/u/dlssnr-amd-logs-1.tar.gz")
        self.assertTrue(jobs.parse_output([("out", "Uninstalled from /g.")])["removed"])

    def test_build_command(self):
        pkg = Package(None)
        pkg.root = Path("/p")
        c = jobs.build_command(pkg, "/g", "install", "reshade-feeder", "CC_SILVER_AARDWOLD", "wilsjo2", True, "/d.zip")
        self.assertEqual(c, ["bash", "/p/install.sh", "/g", "reshade-feeder", "--model", "CC_SILVER_AARDWOLD", "--wilsjo2",
                             "--lumenite", "--dll", "/d.zip"])
        c = jobs.build_command(pkg, "/g", "install", "optiscaler", None, "renodx", True, None)
        self.assertEqual(c, ["bash", "/p/install.sh", "/g", "optiscaler", "--renodx"])  # no lumenite flag on optiscaler
        self.assertEqual(jobs.build_command(pkg, "/g", "remove"), ["bash", "/p/install.sh", "/g", "remove"])
        with self.assertRaises(ValueError):
            jobs.build_command(pkg, "/g", "install", "bogus")


def build_fake_home(home: Path) -> dict:
    """Steam (native + ~/.steam symlink + flatpak + an extra library), Heroic and Lutris installs."""
    ids = {}
    steam = home / ".local" / "share" / "Steam"
    lib2 = home / "SDCard" / "SteamLibrary"
    write(steam / "steamapps" / "libraryfolders.vdf",
          '"libraryfolders"\n{\n "0" { "path" "%s" }\n "1" { "path" "%s" }\n}\n' % (steam, lib2))
    (home / ".steam").mkdir(parents=True)
    os.symlink(steam, home / ".steam" / "steam")

    def app(lib, appid, name, installdir):
        write(lib / "steamapps" / ("appmanifest_%s.acf" % appid),
              '"AppState"\n{\n "appid" "%s"\n "name" "%s"\n "installdir" "%s"\n}\n' % (appid, name, installdir))
        return lib / "steamapps" / "common" / installdir

    ue = app(steam, "1001", "Unreal Game", "UnrealGame")
    make_pe(ue / "UnrealGame.exe", 64, 300000)
    make_pe(ue / "UnrealGame" / "Binaries" / "Win64" / "UnrealGame-Win64-Shipping.exe", 64, 20000)
    old = app(lib2, "1002", "Old 32-bit Game", "OldGame")
    make_pe(old / "OldGame.exe", 32, 30000)
    make_pe(old / "Redist" / "dxsetup.exe", 32, 900000)
    app(steam, "1493710", "Proton Experimental", "Proton - Experimental").mkdir(parents=True)
    make_pe(steam / "steamapps" / "common" / "Proton - Experimental" / "proton.exe", 64)
    app(steam, "1003", "Not Installed", "Missing")  # manifest without folder: skipped
    write(steam / "appcache" / "librarycache" / "1001" / "header.jpg", "jpg")
    write(steam / "appcache" / "librarycache" / "1001" / "abc123" / "library_600x900.jpg", "jpg")
    write(steam / "appcache" / "librarycache" / "1002_header.jpg", "jpg")

    flat = home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam"
    fg = app(flat, "1004", "Flatpak Game", "FlatGame")
    make_pe(fg / "FlatGame.exe", 64)

    heroic = home / ".config" / "heroic"
    epic = home / "Games" / "Heroic" / "EpicGame"
    make_pe(epic / "EpicGame.exe", 64)
    write(heroic / "legendaryConfig" / "legendary" / "installed.json",
          json.dumps({"abc": {"title": "Epic Game", "install_path": str(epic), "platform": "Windows"}}))
    gog = home / "Games" / "Heroic" / "GogGame"
    make_pe(gog / "GogGame.exe", 32)
    write(heroic / "gog_store" / "installed.json",
          json.dumps({"installed": [{"appName": "777", "install_path": str(gog), "platform": "windows"}]}))
    write(heroic / "store_cache" / "gog_library.json", json.dumps({"games": [{"app_name": "777", "title": "GOG Game"}]}))

    lutris_dir = home / ".local" / "share" / "lutris"
    lg = home / "Games" / "lutris-game" / "drive_c" / "Game"
    make_pe(lg / "Bin" / "LutrisGame.exe", 64)
    lutris_dir.mkdir(parents=True)
    con = sqlite3.connect(str(lutris_dir / "pga.db"))
    con.execute("CREATE TABLE games (id INTEGER, slug TEXT, name TEXT, runner TEXT, directory TEXT, configpath TEXT, installed INTEGER)")
    con.execute("INSERT INTO games VALUES (1, 'lutris-game', 'Lutris Game', 'wine', ?, 'lutris-game-1', 1)", (str(home / "Games" / "lutris-game"),))
    con.execute("INSERT INTO games VALUES (2, 'native', 'Native Game', 'linux', '/nonexistent', 'native-1', 1)")
    con.commit()
    con.close()
    write(home / ".config" / "lutris" / "games" / "lutris-game-1.yml", "game:\n  exe: %s\n  prefix: x\n" % (lg / "Bin" / "LutrisGame.exe"))
    ids.update(ue=ue, old=old, epic=epic, gog=gog, lutris=lg / "Bin")
    return ids


class TestLibrary(unittest.TestCase):
    def test_scan(self):
        with tempfile.TemporaryDirectory() as td:
            home = Path(td) / "home"
            ids = build_fake_home(home)
            manual = home / "Manual Game"
            make_pe(manual / "Manual.exe", 64)
            games = library.scan_all(home, [str(manual)], {})
            by = {g["id"]: g for g in games}
            self.assertEqual(sorted(by), sorted(["steam:1001", "steam:1002", "steam:1004", "heroic:epic-abc", "heroic:gog-777",
                                                 "lutris:lutris-game", library.manual_game(str(manual))["id"]]))
            ue = by["steam:1001"]
            self.assertEqual(Path(ue["folder"]), (ids["ue"] / "UnrealGame" / "Binaries" / "Win64").resolve())
            self.assertEqual(ue["exeBits"], 64)
            self.assertTrue(ue["art"]["hero"].endswith("1001/header.jpg"))
            self.assertTrue(ue["art"]["thumb"].endswith("library_600x900.jpg"))
            old = by["steam:1002"]
            self.assertEqual((old["exe"], old["exeBits"]), ("OldGame.exe", 32))
            self.assertTrue(old["art"]["hero"].endswith("1002_header.jpg"))
            self.assertEqual(by["heroic:gog-777"]["name"], "GOG Game")
            self.assertEqual(Path(by["lutris:lutris-game"]["folder"]), ids["lutris"])
            self.assertEqual([g["name"] for g in games], sorted([g["name"] for g in games], key=str.casefold))
            # an override wins and is used as is
            ov = library.scan_all(home, [], {"steam:1002": str(ids["old"] / "Redist")})
            self.assertEqual(next(g for g in ov if g["id"] == "steam:1002")["folder"], str(ids["old"] / "Redist"))


class TestApiSecurity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.td = tempfile.TemporaryDirectory()
        t = Path(cls.td.name)
        pkg = t / "pkg"
        write(pkg / "install.sh", "#!/bin/bash\necho fake\n")
        (pkg / "dlssnr-amd").mkdir()
        (pkg / "reshade").mkdir()
        write(pkg / "reshade" / "dlssnr_amd.addon32")  # a 32-bit package: no OptiScaler
        cls.home = t / "home"
        build_fake_home(cls.home)
        cls.fx = ServerFixture(pkg, cls.home, t / "cfg" / "launcher.json")
        cls.c = cls.fx.client

    @classmethod
    def tearDownClass(cls):
        cls.fx.close()
        cls.td.cleanup()

    def test_token_required(self):
        self.assertEqual(self.c.call("/api/info", token=False)[0], 403)
        self.assertEqual(self.c.call("/api/info", token="wrong")[0], 403)
        self.assertEqual(self.c.call("/api/install", {"id": "x"}, token=False)[0], 403)
        status, info = self.c.call("/api/info")
        self.assertEqual(status, 200)
        self.assertEqual((info["package"]["arch"], info["package"]["bits"]), ("i686", 32))
        self.assertFalse(info["package"]["routes"]["optiscaler"]["available"])
        status, _ = self.c.call("/api/info?t=" + self.fx.app.token, token=False)
        self.assertEqual(status, 200)

    def test_host_and_origin(self):
        self.assertEqual(self.c.call("/api/info", headers={"Host": "evil.example:%d" % self.fx.app.port})[0], 403)
        self.assertEqual(self.c.call("/api/info", headers={"Origin": "http://evil.example"})[0], 403)
        self.assertEqual(self.c.call("/api/info", headers={"Origin": "http://127.0.0.1:%d" % self.fx.app.port})[0], 200)
        self.assertEqual(self.c.call("/api/info", headers={"Host": "localhost:%d" % self.fx.app.port})[0], 200)

    def test_json_only_posts(self):
        req = urllib.request.Request(self.c.base + "/api/settings", data=b"lang=zh", method="POST",
                                     headers={"X-NR-Token": self.fx.app.token, "Content-Type": "application/x-www-form-urlencoded"})
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(req, timeout=10)
        self.assertEqual(cm.exception.code, 415)

    def test_static_and_index(self):
        status, body = self.c.call("/", token=False, raw=True)
        self.assertEqual(status, 403)
        status, body = self.c.call("/?t=" + self.fx.app.token, token=False, raw=True)
        self.assertEqual(status, 200)
        self.assertIn(b"/app.js", body)
        self.assertEqual(self.c.call("/app.js", token=False, raw=True)[0], 200)
        self.assertEqual(self.c.call("/../server.py", token=False, raw=True)[0], 404)

    def test_library_and_art(self):
        status, r = self.c.call("/api/games")
        self.assertEqual(status, 200)
        ids = {g["id"]: g for g in r["games"]}
        self.assertIn("steam:1001", ids)
        self.assertTrue(ids["steam:1001"]["bitsMismatch"])   # 64-bit game, 32-bit package
        self.assertFalse(ids["steam:1002"]["bitsMismatch"])
        self.assertEqual(ids["steam:1001"]["art"], {"hero": True, "thumb": True})  # flags only, no paths
        status, data = self.c.call("/api/art?id=steam:1001&kind=hero", raw=True)
        self.assertEqual((status, data), (200, b"jpg"))
        self.assertEqual(self.c.call("/api/art?id=heroic:epic-abc&kind=hero")[0], 404)

    def test_settings_and_browse(self):
        status, r = self.c.call("/api/settings", {"lang": "zh", "theme": "dark"})
        self.assertEqual((status, r["settings"]["lang"], r["settings"]["theme"]), (200, "zh", "dark"))
        self.assertEqual(self.c.call("/api/settings", {"theme": "neon"})[1]["settings"]["theme"], "dark")
        status, r = self.c.call("/api/browse?mode=dir&path=" + urllib.request.quote(str(self.home)))
        self.assertEqual(status, 200)
        self.assertIn("Games", [e["name"] for e in r["entries"]])
        self.assertTrue(any(p["key"] == "steam" for p in r["places"]))

    def test_route_refused_on_i686(self):
        status, r = self.c.call("/api/install", {"id": "steam:1002", "route": "optiscaler"})
        self.assertEqual((status, r["error"]), (400, "route_unavailable"))
        status, r = self.c.call("/api/install", {"id": "steam:1002", "route": "reshade-feeder"})
        self.assertEqual((status, r["error"]), (400, "model_required"))
        self.assertIn("nvngx_dlssnr.dll", r["message"])
        status, r = self.c.call("/api/install", {"id": "steam:1002", "route": "reshade-feeder", "dll": "/nonexistent.dll"})
        self.assertEqual((status, r["error"]), (400, "dll_invalid"))


class TestDesktopEntry(unittest.TestCase):
    def test_install_remove(self):
        with tempfile.TemporaryDirectory() as td:
            old = os.environ.get("XDG_DATA_HOME")
            os.environ["XDG_DATA_HOME"] = td
            try:
                entry = write(Path(td) / "pkg dir" / "Opti$Scaler" / "OptiScalerNR-Launcher.sh")
                path = desktop.install_desktop(entry, HERE / "web" / "icon.svg", "1.0", "x86_64")
                text = path.read_text()
                self.assertIn('Exec="%s"' % str(entry).replace("$", "\\$"), text)
                self.assertTrue((Path(td) / "icons/hicolor/scalable/apps/optiscaler-nr-launcher.svg").is_file())
                self.assertEqual(len(desktop.remove_desktop()), 2)
            finally:
                if old is None:
                    os.environ.pop("XDG_DATA_HOME", None)
                else:
                    os.environ["XDG_DATA_HOME"] = old


# ------------------------------------------------------------------------------------------------
# end-to-end against real packages
# ------------------------------------------------------------------------------------------------
def hardlink_copy(src: Path, dst: Path) -> None:
    def link(s, d):
        try:
            os.link(s, d)
        except OSError:
            shutil.copy2(s, d)
    shutil.copytree(str(src), str(dst), symlinks=True, copy_function=link)


def replace_file(path: Path, data: bytes, mode: int = 0o755) -> None:
    """Never write through a hard link: unlink first so the original package stays untouched."""
    path.unlink()
    path.write_bytes(data)
    path.chmod(mode)


class E2E(unittest.TestCase):
    def test_packages(self):
        if not PACKAGES:
            self.skipTest("no package folder (run from an extracted package or pass --package)")
        for src in PACKAGES:
            with self.subTest(package=str(src)):
                self.run_package(Path(src))

    def run_package(self, src: Path):
        print("\n[e2e] package %s" % src, flush=True)
        src_install = (src / "install.sh").read_bytes()
        work = Path(tempfile.mkdtemp(prefix="nr-e2e-", dir=os.environ.get("NR_E2E_TMP")))
        saved_env = {k: os.environ.get(k) for k in ("HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME")}
        fixtures = []
        try:
            home = work / "home"
            home.mkdir()
            os.environ.update(HOME=str(home), XDG_CONFIG_HOME=str(work / "cfg"), XDG_CACHE_HOME=str(work / "cache"),
                              XDG_DATA_HOME=str(work / "data"))
            pkg = work / "pkg"
            hardlink_copy(src, pkg)
            meta = Package(pkg)
            bits = meta.bits
            routes = [r for r, v in meta.routes().items() if v["available"]]
            self.assertEqual("optiscaler" in routes, bits == 64)

            g64 = home / "Games" / "Game 64"
            make_pe(g64 / "Game64.exe", 64, 200000)
            write(g64 / "Game64_Data" / "level0.dat", "level")
            write(g64 / "readme.txt", "hello")
            g32 = home / "Games" / "Game 32"
            make_pe(g32 / "Game32.exe", 32, 150000)
            write(g32 / "data.pak", "pak")
            own = g64 if bits == 64 else g32
            other = g32 if bits == 64 else g64
            orig = {own: snapshot(own), other: snapshot(other)}
            state_path = work / "cfg" / "optiscaler-nr" / "launcher.json"

            def start():
                fx = ServerFixture(pkg, home, state_path)
                fixtures.append(fx)
                c = fx.client
                for folder in (own, other):
                    status, r = c.call("/api/library/add", {"path": str(folder)})
                    self.assertEqual(status, 200, r)
                ids = {}
                for g in c.call("/api/games")[1]["games"]:
                    ids[Path(g["folder"])] = g
                return fx, c, ids

            # --- 1. no model in the package: refused by the API, and install.sh's own message surfaced
            fx, c, ids = start()
            gid = ids[own]["id"]
            status, r = c.call("/api/install", {"id": gid, "route": "reshade-feeder"})
            self.assertEqual((status, r["error"]), (400, "model_required"))
            status, r = c.call("/api/install", {"id": gid, "route": "reshade-feeder", "skipModelCheck": True})
            self.assertEqual(status, 200, r)
            job = c.wait_job(r["job"]["id"])
            self.assertEqual(job["state"], "failed")
            self.assertIn("no model file", job["result"]["error"])
            self.assertEqual(snapshot(own), orig[own])
            print("[e2e]   no-model install refused: %s" % job["result"]["error"].splitlines()[0], flush=True)
            fx.close()

            # --- 2. dummy model in the temporary copy only
            dummy_aard = os.urandom(65536)
            dummy_small = os.urandom(65536)
            (pkg / "dlssnr-amd" / "CC_SILVER_AARDWOLD.bin").write_bytes(dummy_aard)
            (pkg / "dlssnr-amd" / "CC_SMALL_FP8.bin").write_bytes(dummy_small)
            text = src_install.decode("utf-8")
            text, n1 = re.subn(r"^model_sha256=[0-9a-f]{64}$", "model_sha256=" + hashlib.sha256(dummy_aard).hexdigest(), text, flags=re.M)
            text, n2 = re.subn(r"^small_sha256=[0-9a-f]{64}$", "small_sha256=" + hashlib.sha256(dummy_small).hexdigest(), text, flags=re.M)
            self.assertEqual((n1, n2), (1, 1))
            replace_file(pkg / "install.sh", text.encode("utf-8"))
            self.assertEqual((src / "install.sh").read_bytes(), src_install, "the source package must stay untouched")

            fx, c, ids = start()
            info = c.call("/api/info")[1]
            self.assertTrue(info["model"]["complete"], info["model"])
            gid, oid = ids[own]["id"], ids[other]["id"]
            self.assertFalse(ids[own]["bitsMismatch"])
            self.assertTrue(ids[other]["bitsMismatch"])

            if bits == 32:
                status, r = c.call("/api/install", {"id": gid, "route": "optiscaler"})
                self.assertEqual((status, r["error"]), (400, "route_unavailable"))
                p = subprocess.run(["bash", str(pkg / "install.sh"), str(own), "optiscaler", "--model", "CC_SMALL_FP8"],
                                   stdin=subprocess.DEVNULL, capture_output=True, text=True)
                self.assertEqual(p.returncode, 1)
                self.assertIn("only in the 64-bit package", p.stderr)
                self.assertEqual(snapshot(own), orig[own])
                print("[e2e]   i686: optiscaler refused (API 400 + installer: %s)" % p.stderr.strip(), flush=True)

            expected_launch = {"optiscaler": LAUNCH_DXGI, "reshade-feeder": LAUNCH_DXGI,
                               "reshade-feeder-vulkan": LAUNCH_VK, "reshade-feeder-dx9": LAUNCH_VK}
            for i, route in enumerate(routes):
                t0 = time.time()
                job = self.install(c, gid, route, "CC_SMALL_FP8", "renodx")
                g = c.call("/api/game?refresh=1&id=" + urllib.request.quote(gid))[1]["game"]
                self.assertTrue(g["status"]["installed"])
                self.assertEqual(g["status"]["route"], route)
                self.assertEqual(g["status"]["model"], "CC_SMALL_FP8")
                self.assertEqual(g["status"]["style"], "renodx")
                self.assertEqual(g["record"]["launch"], expected_launch[route])
                self.assertEqual(job["result"]["launch"], expected_launch[route])
                self.check_manifest(own)
                # reinstall: other network and style
                job = self.install(c, gid, route, "CC_SILVER_AARDWOLD", "wilsjo2")
                g = c.call("/api/game?refresh=1&id=" + urllib.request.quote(gid))[1]["game"]
                self.assertEqual((g["status"]["route"], g["status"]["model"], g["status"]["style"]),
                                 (route, "CC_SILVER_AARDWOLD", "wilsjo2"))
                self.assertIn("Found a previous installation, removing it first.", [ln["t"] for ln in job["lines"]])
                self.check_manifest(own)
                # remove: alternate between the launcher API and uninstall.sh
                if i % 2 == 0:
                    status, r = c.call("/api/uninstall", {"id": gid})
                    self.assertEqual(status, 200, r)
                    job = c.wait_job(r["job"]["id"])
                    self.assertEqual(job["state"], "done", job)
                    self.assertTrue(job["result"]["removed"])
                    how = "API"
                else:
                    p = subprocess.run(["bash", str(pkg / "uninstall.sh"), str(own)], stdin=subprocess.DEVNULL,
                                       capture_output=True, text=True)
                    self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
                    self.assertIn("Uninstalled from", p.stdout)
                    how = "uninstall.sh"
                self.assertEqual(snapshot(own), orig[own], "game folder not restored after %s (%s)" % (route, how))
                st = json.loads(state_path.read_text())
                self.assertNotIn(str(own), st["installs"])
                print("[e2e]   %-22s install -> status -> reinstall -> remove (%s) OK  %.1fs" % (route, how, time.time() - t0), flush=True)

            # logs
            status, r = c.call("/api/logs", {"id": gid})
            job = c.wait_job(r["job"]["id"])
            self.assertEqual(job["state"], "done", job)
            self.assertTrue(Path(job["result"]["logs"]).is_file())
            Path(job["result"]["logs"]).unlink()
            self.assertEqual(snapshot(own), orig[own])

            # wrong-bitness game: installs with the installer's warning; then uninstall.sh --all removes both
            self.install(c, gid, "reshade-feeder", None, "renodx")
            job = self.install(c, oid, "reshade-feeder-vulkan", None, "renodx")
            self.assertTrue(any("DLLs of the other bitness will not load" in ln["t"] for ln in job["lines"]))
            st = json.loads(state_path.read_text())
            self.assertEqual(sorted(st["installs"]), sorted([str(own), str(other)]))
            p = subprocess.run(["bash", str(pkg / "uninstall.sh"), "--all"], stdin=subprocess.DEVNULL, capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
            self.assertEqual(snapshot(own), orig[own])
            self.assertEqual(snapshot(other), orig[other])
            self.assertEqual(json.loads(state_path.read_text())["installs"], {})
            print("[e2e]   uninstall.sh --all restored both game folders", flush=True)
            fx.close()
            p = subprocess.run(["bash", str(pkg / "uninstall.sh"), "--purge-settings", "--desktop"], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stderr)
            self.assertFalse(state_path.parent.exists())
            self.assertEqual((src / "install.sh").read_bytes(), src_install)
            self.assertFalse((src / "dlssnr-amd" / "CC_SILVER_AARDWOLD.bin").exists())
        finally:
            for fx in fixtures:
                try:
                    fx.close()
                except Exception:
                    pass
            for k, v in saved_env.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
            shutil.rmtree(str(work), ignore_errors=True)

    def install(self, c, gid, route, model, style):
        status, r = c.call("/api/install", {"id": gid, "route": route, "model": model, "style": style, "lumenite": False})
        self.assertEqual(status, 200, r)
        job = c.wait_job(r["job"]["id"])
        if job["state"] != "done":
            print("\n".join(ln["t"] for ln in job["lines"][-30:]))
        self.assertEqual(job["state"], "done", job["result"])
        return job

    def check_manifest(self, folder: Path):
        entries = library.read_manifest(folder)
        self.assertIn("dlssnr-amd-install.txt", entries)
        for e in entries:
            if e.rstrip("/") in library.RUNTIME_FILES:  # logs written by the game at run time
                continue
            self.assertTrue((folder / e.rstrip("/")).exists(), "manifest entry missing: %s" % e)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--package", action="append", default=[], help="package folder for the end-to-end test (repeatable)")
    ap.add_argument("--unit-only", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    args, rest = ap.parse_known_args()
    if not args.unit_only:
        PACKAGES.extend(Path(p).resolve() for p in args.package)
        if not PACKAGES:
            own = find_package(None)
            if own and is_package_root(own):
                PACKAGES.append(own)
    suite = unittest.TestLoader().loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2 if args.verbose else 1).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
