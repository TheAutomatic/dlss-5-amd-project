"""Game library discovery for Linux: Steam, Heroic, Lutris and manually added folders.

Also: a tiny VDF (Valve KeyValues) parser, the "where is the real game exe" heuristic, PE
bitness detection and detection of an existing DLSSNR-AMD installation in a game folder.
Everything here is best effort: a broken launcher config must never break the library.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import struct
import time
from pathlib import Path

MANIFEST = "dlssnr-amd-install.txt"

# ---------------------------------------------------------------------------------------------
# VDF
# ---------------------------------------------------------------------------------------------
_VDF_TOKEN = re.compile(r'\s*(?:(//[^\n]*)|"((?:[^"\\]|\\.)*)"|(\{)|(\})|([^\s{}"]+))', re.S)
_VDF_ESC = {"n": "\n", "t": "\t", "\\": "\\", '"': '"'}


def parse_vdf(text: str) -> dict:
    """Parse text KeyValues into nested dicts. Keys are lower-cased (Steam is case-insensitive);
    later duplicates win. Conditional suffixes like ``[$WIN32]`` are kept as plain tokens."""
    root: dict = {}
    stack = [root]
    key = None
    pos = 0
    n = len(text)
    while pos < n:
        m = _VDF_TOKEN.match(text, pos)
        if not m or m.end() == pos:
            break
        pos = m.end()
        comment, quoted, open_b, close_b, bare = m.groups()
        if comment is not None:
            continue
        if open_b:
            child: dict = {}
            if key is not None:
                stack[-1][key.lower()] = child
            stack.append(child)
            key = None
            continue
        if close_b:
            if len(stack) > 1:
                stack.pop()
            key = None
            continue
        tok = quoted if quoted is not None else bare
        if tok is None:
            continue
        if quoted is not None:
            tok = re.sub(r"\\(.)", lambda mm: _VDF_ESC.get(mm.group(1), "\\" + mm.group(1)), tok)
        elif tok.startswith("[$") and key is None:
            continue  # conditional after a value
        if key is None:
            key = tok
        else:
            stack[-1][key.lower()] = tok
            key = None
    return root


def _read_vdf(path: Path) -> dict:
    try:
        return parse_vdf(path.read_text(encoding="utf-8", errors="replace"))
    except OSError:
        return {}


# ---------------------------------------------------------------------------------------------
# PE bitness
# ---------------------------------------------------------------------------------------------
PE_MACHINES = {0x14C: 32, 0x8664: 64, 0xAA64: 64}


def pe_bits(path) -> int | None:
    """32 / 64 for a Windows PE image, None when unreadable or not PE."""
    try:
        with open(path, "rb") as f:
            head = f.read(4096)
            if len(head) < 0x40 or head[:2] != b"MZ":
                return None
            off = struct.unpack_from("<I", head, 0x3C)[0]
            if off + 6 > len(head):
                f.seek(off)
                sig = f.read(6)
            else:
                sig = head[off:off + 6]
            if len(sig) < 6 or sig[:4] != b"PE\0\0":
                return None
            return PE_MACHINES.get(struct.unpack_from("<H", sig, 4)[0])
    except (OSError, struct.error):
        return None


def folder_bits(folder: Path) -> list:
    """Bitness of every *.exe directly in folder (what install.sh itself looks at)."""
    seen = set()
    try:
        for e in os.scandir(folder):
            if e.is_file() and e.name.lower().endswith(".exe"):
                b = pe_bits(e.path)
                if b:
                    seen.add(b)
    except OSError:
        pass
    return sorted(seen)


# ---------------------------------------------------------------------------------------------
# Finding the folder with the real game exe
# ---------------------------------------------------------------------------------------------
_SKIP_EXE = re.compile(
    r"(?i)^(unins\d*|.*setup.*|.*install.*|vc_?redist.*|dxwebsetup|dxsetup|.*crash.*|.*report.*|"
    r".*launcher.*|.*redist.*|.*uploader.*|easyanticheat.*|eac.*|battleye.*|be_service.*|.*prereq.*|"
    r"dotnet.*|ndp\d+.*|physx.*|oalinst|.*cefprocess.*|.*helper.*|.*updater?.*|.*patcher.*|7z.*|"
    r"unitycrashhandler.*|quicksfv|.*touchup.*|.*cleanup.*|cefsharp.*|crs-.*|zfgamebrowser|"
    r"subprocess|python.*|steamerrorreporter.*|steamservice)\.exe$")
_SKIP_DIRS = {
    "_commonredist", "commonredist", "redist", "_redist", "redistributables", "__installer", "installer",
    "installers", "directx", "vcredist", "easyanticheat", "battleye", "support", "prerequisites",
    "dotnetfx", "physx", "thirdparty", "extras", "__macosx", "crashreportclient", "dxsetup", "eac",
    "steamworks shared", "binaries_linux", "linux", "mono", "monobleedingedge", "jre", "java",
}
_MAX_DEPTH = 4
_MAX_ENTRIES = 6000
_exe_cache: dict = {}


def _shipping_exe(root: Path):
    """Unreal Engine: <project>/Binaries/Win64|Win32/*-Shipping.exe (not Engine/)."""
    best = None
    try:
        for proj in os.scandir(root):
            if not proj.is_dir() or proj.name.lower() == "engine":
                continue
            for arch in ("Win64", "WinGDK", "Win32"):
                d = Path(proj.path) / "Binaries" / arch
                try:
                    for e in os.scandir(d):
                        if e.is_file() and e.name.lower().endswith("-shipping.exe"):
                            size = e.stat().st_size
                            if best is None or size > best[1]:
                                best = (Path(e.path), size)
                except OSError:
                    continue
    except OSError:
        pass
    return best[0] if best else None


def find_game_exe(install_dir) -> Path | None:
    """Return the exe that most likely is the game (its folder is where to install).

    Prefer an Unreal ``*-Shipping.exe``; otherwise walk a few levels and take the largest exe that
    is not a redistributable / launcher / crash handler (falling back to the largest of all).
    """
    root = Path(install_dir)
    try:
        st = root.stat()
    except OSError:
        return None
    key = (str(root), st.st_mtime_ns)
    if key in _exe_cache:
        return _exe_cache[key]
    exe = _shipping_exe(root)
    if exe is None:
        good, anyexe = None, None
        count = 0
        stack = [(root, 0)]
        while stack and count < _MAX_ENTRIES:
            d, depth = stack.pop()
            try:
                entries = list(os.scandir(d))
            except OSError:
                continue
            for e in entries:
                count += 1
                try:
                    if e.is_dir(follow_symlinks=False):
                        if depth < _MAX_DEPTH and e.name.lower() not in _SKIP_DIRS and not e.name.startswith("."):
                            if not (e.name.lower() == "engine" and depth == 0):
                                stack.append((Path(e.path), depth + 1))
                    elif e.name.lower().endswith(".exe") and e.is_file():
                        size = e.stat().st_size
                        cand = (size, -depth, Path(e.path))
                        if anyexe is None or cand[:2] > anyexe[:2]:
                            anyexe = cand
                        if not _SKIP_EXE.match(e.name) and (good is None or cand[:2] > good[:2]):
                            good = cand
                except OSError:
                    continue
        pick = good or anyexe
        exe = pick[2] if pick else None
    _exe_cache[key] = exe
    return exe


def direct_exe(folder: Path):
    """(exe, looks_like_the_game) for the largest exe directly in folder; (None, False) if there is none."""
    direct = []
    try:
        for e in os.scandir(folder):
            if e.is_file() and e.name.lower().endswith(".exe"):
                direct.append((e.stat().st_size, Path(e.path)))
    except OSError:
        return None, False
    if not direct:
        return None, False
    good = [d for d in direct if not _SKIP_EXE.match(d[1].name)]
    return (max(good)[1], True) if good else (max(direct)[1], False)


def resolve_game_folder(path) -> tuple:
    """(folder, exe) for a user-picked folder: a real exe directly in it wins unless it is the top
    folder of an Unreal game; otherwise the game exe found below it; else whatever exe is there."""
    folder = Path(path)
    shipping = _shipping_exe(folder)
    if shipping:
        return shipping.parent, shipping
    exe, good = direct_exe(folder)
    if exe and good:
        return folder, exe
    deep = find_game_exe(folder)
    if deep and not _SKIP_EXE.match(deep.name):
        return deep.parent, deep
    if exe:
        return folder, exe
    return (deep.parent if deep else folder), deep


# ---------------------------------------------------------------------------------------------
# Installation detection
# ---------------------------------------------------------------------------------------------
RUNTIME_FILES = {"OptiScaler.log", "dlssnr-amd.log", "ReShade.log"}


def read_manifest(folder: Path) -> list:
    try:
        text = (Path(folder) / MANIFEST).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    return [line.strip() for line in text.splitlines() if line.strip()]


def ini_value(text: str, section: str, key: str) -> str | None:
    """First ``key = value`` inside ``[section]`` (line based; ';' / '#' lines are comments)."""
    current = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line[0] in ";#":
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1].strip()
            continue
        if current == section and "=" in line:
            k, v = line.split("=", 1)
            if k.strip() == key:
                return v.strip()
    return None


def detect_install(folder) -> dict:
    """Is DLSSNR-AMD installed in this folder, and by which route?

    ``installed`` follows the installer's manifest. The route is inferred from the files:
    dxgi.dll + OptiScaler.ini => optiscaler, vulkan-1.dll + winevulkan.dll => vulkan (dx9 when the
    preset carries the D3D9 depth direction), dxgi.dll + ReShade.ini => reshade-feeder.
    ``foreign`` flags a proxy DLL that is not ours (another mod) - the installer would overwrite it.
    """
    folder = Path(folder)
    entries = read_manifest(folder)
    has = lambda n: (folder / n).exists()  # noqa: E731
    route = None
    if entries:
        names = {e.rstrip("/") for e in entries}
        if "OptiScaler.ini" in names or (has("dxgi.dll") and has("OptiScaler.ini") and "dxgi.dll" in names):
            route = "optiscaler"
        elif "winevulkan.dll" in names or ("vulkan-1.dll" in names and has("winevulkan.dll")):
            route = "reshade-feeder-vulkan"
            try:
                preset = (folder / "ReShadePreset.ini").read_text(encoding="utf-8", errors="replace")
                if "RESHADE_DEPTH_INPUT_IS_REVERSED=0" in preset:
                    route = "reshade-feeder-dx9"
            except OSError:
                pass
        elif "ReShade.ini" in names or (has("dxgi.dll") and has("ReShade.ini")):
            route = "reshade-feeder"
    foreign = None
    if not entries:
        for name in ("dxgi.dll", "d3d11.dll", "d3d12.dll", "dinput8.dll", "winmm.dll", "version.dll", "vulkan-1.dll"):
            if has(name):
                foreign = name
                break
    model = None
    style = None
    try:
        ini = (folder / "dlssnr-amd.ini").read_text(encoding="utf-8", errors="replace")
        model = ini_value(ini, "Network", "Model")
        # every route's ini: [RenoDX] Enabled = 1 is the RenoDX style, 0 the wilsjo2 style
        enabled = ini_value(ini, "RenoDX", "Enabled")
        style = {"1": "renodx", "0": "wilsjo2"}.get(enabled or "")
    except OSError:
        pass
    return {
        "installed": bool(entries),
        "route": route,
        "entries": len(entries),
        "foreign": foreign,
        "model": model,
        "style": style,
    }


# ---------------------------------------------------------------------------------------------
# Steam
# ---------------------------------------------------------------------------------------------
# Tools and runtimes that are not games
_STEAM_SKIP_APPIDS = {"228980", "1070560", "1391110", "1628350", "1493710", "1887720", "2180100", "2348590",
                      "1161040", "1580130", "961940", "1420170", "1245040", "1113280", "1054830", "858280",
                      "930400", "2805730", "3658110"}
_STEAM_SKIP_NAME = re.compile(r"(?i)^(proton\b|steam linux runtime|steamworks common|steamvr|"
                              r"proton easyanticheat|proton battleye|steam audio)")


def steam_roots(home: Path) -> list:
    cands = [
        home / ".steam" / "steam",
        home / ".steam" / "root",
        home / ".local" / "share" / "Steam",
        home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam",
        home / ".var" / "app" / "com.valvesoftware.Steam" / "data" / "Steam",
        home / "snap" / "steam" / "common" / ".local" / "share" / "Steam",
    ]
    out, seen = [], set()
    for c in cands:
        try:
            if not (c / "steamapps").is_dir():
                continue
            real = c.resolve()
        except OSError:
            continue
        if real not in seen:
            seen.add(real)
            out.append(real)
    return out


def steam_libraries(root: Path) -> list:
    libs = [root]
    for vdf in (root / "steamapps" / "libraryfolders.vdf", root / "config" / "libraryfolders.vdf"):
        data = _read_vdf(vdf).get("libraryfolders", {})
        if not isinstance(data, dict):
            continue
        for k, v in data.items():
            path = None
            if isinstance(v, dict):
                path = v.get("path")
            elif isinstance(v, str) and k.isdigit():
                path = v  # pre-2021 format: "1" "/path"
            if path:
                libs.append(Path(path))
    out, seen = [], set()
    for lib in libs:
        try:
            if not (lib / "steamapps").is_dir():
                continue
            real = lib.resolve()
        except OSError:
            continue
        if real not in seen:
            seen.add(real)
            out.append(real)
    return out


_ART_NAMES = {
    "hero": ("library_hero.jpg", "library_hero.png", "header.jpg", "library_header.jpg", "capsule_616x353.jpg"),
    "thumb": ("library_600x900.jpg", "library_capsule.jpg", "header.jpg", "library_header.jpg", "capsule_231x87.jpg"),
}


def steam_art(root: Path, appid: str) -> dict:
    """Locate cached library art. New Steam: librarycache/<appid>/[<hash>/]name; old: <appid>_name."""
    cache = root / "appcache" / "librarycache"
    found = {}
    files = {}
    d = cache / appid
    try:
        if d.is_dir():
            for e in os.scandir(d):
                if e.is_file():
                    files.setdefault(e.name.lower(), Path(e.path))
                elif e.is_dir():
                    try:
                        for e2 in os.scandir(e.path):
                            if e2.is_file():
                                files.setdefault(e2.name.lower(), Path(e2.path))
                    except OSError:
                        pass
    except OSError:
        pass
    for kind, names in _ART_NAMES.items():
        for name in names:
            p = files.get(name)
            if p is None:
                flat = cache / ("%s_%s" % (appid, name))
                if flat.is_file():
                    p = flat
            if p is not None:
                found[kind] = str(p)
                break
    return found


def scan_steam(home: Path) -> list:
    games, seen = [], set()
    for root in steam_roots(home):
        for lib in steam_libraries(root):
            try:
                acfs = sorted((lib / "steamapps").glob("appmanifest_*.acf"))
            except OSError:
                continue
            for acf in acfs:
                app = _read_vdf(acf).get("appstate", {})
                if not isinstance(app, dict):
                    continue
                appid = str(app.get("appid", "")).strip()
                name = str(app.get("name") or "").strip()
                installdir = str(app.get("installdir") or "").strip()
                if not appid or not installdir or appid in seen:
                    continue
                if appid in _STEAM_SKIP_APPIDS or _STEAM_SKIP_NAME.match(name):
                    continue
                inst = lib / "steamapps" / "common" / installdir
                if not inst.is_dir():
                    continue
                seen.add(appid)
                games.append({
                    "id": "steam:" + appid,
                    "source": "steam",
                    "appid": appid,
                    "name": name or installdir,
                    "installDir": str(inst),
                    "art": steam_art(root, appid),
                })
    return games


# ---------------------------------------------------------------------------------------------
# Heroic (Epic via legendary, GOG, Amazon via nile) and Lutris - best effort
# ---------------------------------------------------------------------------------------------
def _load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8", errors="replace"))
    except (OSError, ValueError):
        return None


def _heroic_roots(home: Path) -> list:
    return [p for p in (home / ".config" / "heroic",
                        home / ".var" / "app" / "com.heroicgameslauncher.hgl" / "config" / "heroic") if p.is_dir()]


def scan_heroic(home: Path) -> list:
    games, seen = [], set()

    def add(app, title, path, store):
        if not path or app in seen:
            return
        p = Path(path)
        if not p.is_dir():
            return
        seen.add(app)
        games.append({"id": "heroic:%s" % app, "source": "heroic", "store": store,
                      "name": title or p.name, "installDir": str(p), "art": {}})

    legendary_files = [home / ".config" / "legendary" / "installed.json"]
    for root in _heroic_roots(home):
        legendary_files.insert(0, root / "legendaryConfig" / "legendary" / "installed.json")
        gog_titles = {}
        lib = _load_json(root / "store_cache" / "gog_library.json") or {}
        for g in (lib.get("games") or []) if isinstance(lib, dict) else []:
            if isinstance(g, dict) and g.get("app_name"):
                gog_titles[str(g["app_name"])] = g.get("title")
        gog = _load_json(root / "gog_store" / "installed.json") or {}
        for g in (gog.get("installed") or []) if isinstance(gog, dict) else []:
            if isinstance(g, dict) and g.get("platform", "windows") == "windows":
                app = str(g.get("appName") or "")
                add("gog-" + app, gog_titles.get(app), g.get("install_path"), "gog")
        nile = _load_json(root / "nile_config" / "nile" / "installed.json")
        for g in nile if isinstance(nile, list) else []:
            if isinstance(g, dict):
                add("amazon-" + str(g.get("id")), None, g.get("path"), "amazon")
    for f in legendary_files:
        data = _load_json(f)
        if isinstance(data, dict):
            for app, g in data.items():
                if isinstance(g, dict) and g.get("platform", "Windows") in ("Windows", "Win32"):
                    add("epic-" + str(app), g.get("title"), g.get("install_path"), "epic")
    return games


def scan_lutris(home: Path) -> list:
    """Lutris keeps games in pga.db; Wine games' exe path is in games/<configpath>.yml."""
    games = []
    db_paths = [home / ".local" / "share" / "lutris" / "pga.db",
                home / ".var" / "app" / "net.lutris.Lutris" / "data" / "lutris" / "pga.db"]
    for db in db_paths:
        if not db.is_file():
            continue
        try:
            import sqlite3
            con = sqlite3.connect("file:%s?mode=ro" % db, uri=True, timeout=1)
            try:
                rows = con.execute("SELECT slug, name, runner, directory, configpath FROM games WHERE installed = 1").fetchall()
            finally:
                con.close()
        except Exception:  # sqlite3 missing, locked or schema changed
            continue
        cfg_dirs = [db.parent / "games", home / ".config" / "lutris" / "games",
                    db.parent.parent.parent / "config" / "lutris" / "games"]
        for slug, name, runner, directory, configpath in rows:
            if runner and runner not in ("wine", "proton", "umu"):
                continue
            exe_dir = None
            for cd in cfg_dirs:
                yml = cd / ("%s.yml" % configpath) if configpath else None
                if yml and yml.is_file():
                    try:
                        m = re.search(r"^\s+exe:\s*['\"]?(.+?)['\"]?\s*$", yml.read_text(encoding="utf-8", errors="replace"), re.M)
                    except OSError:
                        m = None
                    if m:
                        exe = Path(os.path.expanduser(m.group(1)))
                        if not exe.is_absolute() and directory:
                            exe = Path(directory) / exe
                        if exe.is_file():
                            exe_dir = exe.parent
                    break
            inst = exe_dir or (Path(directory) if directory else None)
            if inst and inst.is_dir():
                games.append({"id": "lutris:%s" % slug, "source": "lutris", "name": name or slug,
                              "installDir": str(inst), "art": {}, "exeHint": bool(exe_dir)})
    return games


def manual_game(path: str) -> dict:
    p = Path(path)
    gid = "manual:" + hashlib.sha1(str(p).encode("utf-8")).hexdigest()[:12]
    return {"id": gid, "source": "manual", "name": p.name or str(p), "installDir": str(p), "art": {}, "manual": True}


# ---------------------------------------------------------------------------------------------
# The whole library
# ---------------------------------------------------------------------------------------------
def scan_all(home: Path | None = None, manual: list = (), overrides: dict | None = None) -> list:
    home = Path(home) if home else Path.home()
    overrides = overrides or {}
    raw = []
    for scanner in (scan_steam, scan_heroic, scan_lutris):
        try:
            raw.extend(scanner(home))
        except Exception:  # never let one launcher break the library
            pass
    for m in manual:
        raw.append(manual_game(m))
    games = []
    seen_folders = set()
    for g in raw:
        enrich(g, overrides.get(g["id"]))
        key = g["folder"]
        if key in seen_folders and g["source"] == "manual":
            continue  # a manual entry duplicating a scanned game
        seen_folders.add(key)
        games.append(g)
    games.sort(key=lambda g: (g["name"].casefold(), g["id"]))
    return games


def enrich(g: dict, override: str | None = None) -> dict:
    """Add folder / exe / bitness / install status to a raw library entry."""
    started = time.monotonic()
    if override and Path(override).is_dir():
        folder = Path(override)  # the user's explicit choice is used as is
        exe = direct_exe(folder)[0]
        g["override"] = override
    elif g.get("source") in ("manual", "lutris"):
        folder, exe = resolve_game_folder(g["installDir"])
    else:
        exe = find_game_exe(g["installDir"])
        folder = exe.parent if exe else Path(g["installDir"])
    g["folder"] = str(folder)
    g["exe"] = exe.name if exe else None
    g["exeBits"] = pe_bits(exe) if exe else None
    g["folderBits"] = folder_bits(folder)
    g["status"] = detect_install(folder)
    g["missing"] = not folder.is_dir()
    g["scanMs"] = int((time.monotonic() - started) * 1000)
    return g
