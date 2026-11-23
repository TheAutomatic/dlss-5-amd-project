"""OpenRR (DLSS Ray Reconstruction) and OptiPatcher addon deployment for the Linux x86_64 OptiScaler route.

Upstream Mochizuki's ``install.sh`` stays byte-identical to upstream and handles OptiScaler + DLSSNR-AMD
(Mochizuki NR). On x86_64 packages, this module overlays ``openrr-amd.asi`` (+ ``openrr-amd/`` +
``openrr-amd.ini``) and ``OptiPatcher.asi`` into ``OptiScaler/plugins/``, caches/copies the user-supplied
``nvngx_dlssd.dll``, patches ``OptiScaler.ini`` spoofing/plugin keys, and appends OpenRR's paths to
``dlssnr-amd-install.txt`` so upstream ``install.sh <game> remove`` and ``uninstall.sh`` remove everything
cleanly.

It can also deploy OptiScaler + OpenRR without Mochizuki NR (when the user turns NR off and does not have
``nvngx_dlssnr.dll``), writing the same ``dlssnr-amd-install.txt`` manifest format.
"""
from __future__ import annotations

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Callable, Optional

from .package import Package

MANIFEST = "dlssnr-amd-install.txt"
OPENRR_MANIFEST_ENTRIES = (
    "openrr-amd/",
    "openrr-amd.ini",
    "openrr-amd.log",
    "openrr-control.bin",
    "openrr-amd-crash.dmp",
)


def is_pe64_bytes(data: bytes) -> bool:
    """True when ``data`` starts with a valid AMD64 (x86_64) Windows PE header."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        return False
    (pe_off,) = struct.unpack_from("<I", data, 0x3C)
    if pe_off + 6 > len(data) or data[pe_off:pe_off + 4] != b"PE\x00\x00":
        return False
    (machine,) = struct.unpack_from("<H", data, pe_off + 4)
    return machine == 0x8664


def is_pe64_file(path: Path) -> bool:
    try:
        with open(path, "rb") as f:
            head = f.read(4096)
            if len(head) < 0x40 or head[:2] != b"MZ":
                return False
            (pe_off,) = struct.unpack_from("<I", head, 0x3C)
            if pe_off + 6 <= len(head):
                return head[pe_off:pe_off + 4] == b"PE\x00\x00" and struct.unpack_from("<H", head, pe_off + 4)[0] == 0x8664
            f.seek(pe_off)
            sig = f.read(6)
            return len(sig) == 6 and sig[:4] == b"PE\x00\x00" and struct.unpack_from("<H", sig, 4)[0] == 0x8664
    except OSError:
        return False


def _atomic_copy_bytes(data: bytes, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(dest.parent), prefix=".tmp-")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, dest)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _atomic_copy_file(src: Path, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(dest.parent), prefix=".tmp-")
    try:
        with open(src, "rb") as r, os.fdopen(fd, "wb") as w:
            shutil.copyfileobj(r, w)
        os.replace(tmp, dest)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def extract_dlssd(src: Path, dest: Path) -> Path:
    """Validate and copy ``nvngx_dlssd.dll`` from a ``.dll`` file or a ``.zip`` containing it."""
    src = Path(src)
    if not src.is_file():
        raise ValueError("nvngx_dlssd.dll source not found: %s" % src)
    low = src.suffix.lower()
    if low == ".dll":
        if not is_pe64_file(src):
            raise ValueError("Not a valid 64-bit Windows DLL: %s" % src)
        _atomic_copy_file(src, dest)
        return dest
    if low == ".zip":
        try:
            with zipfile.ZipFile(src) as z:
                matches = [
                    info for info in z.infolist()
                    if not info.is_dir() and info.filename.rsplit("/", 1)[-1].lower() == "nvngx_dlssd.dll"
                ]
                if not matches:
                    raise ValueError("Zip archive does not contain nvngx_dlssd.dll: %s" % src)
                data = z.read(matches[0])
        except zipfile.BadZipFile as e:
            raise ValueError("Invalid zip archive: %s (%s)" % (src, e))
        if not is_pe64_bytes(data):
            raise ValueError("nvngx_dlssd.dll inside zip is not a valid 64-bit Windows DLL: %s" % src)
        _atomic_copy_bytes(data, dest)
        return dest
    raise ValueError("Expected an nvngx_dlssd.dll or .zip file: %s" % src)


def patch_optiscaler_ini(
    ini_path: Path,
    want_nr: bool,
    want_openrr: bool,
    openrr_spoof: str = "full",
    prior_openrr: bool = False,
) -> None:
    """Patch ``OptiScaler.ini`` in-place while preserving line endings and existing comments."""
    raw = ini_path.read_bytes().decode("utf-8", errors="surrogateescape")
    nl = "\r\n" if "\r\n" in raw else "\n"
    lines = raw.split(nl)

    wanted = {("Plugins", "LoadAsiPlugins"): "true"}
    if want_nr:
        wanted[("Menu", "FGShortcutKey")] = "-1"
    spoof = None
    if want_openrr:
        spoof = "auto" if openrr_spoof == "auto" else "true"
    elif prior_openrr:
        spoof = "auto"
    if spoof:
        for key in ("Dxgi", "Vulkan", "VulkanExtensionSpoofing"):
            wanted[("Spoofing", key)] = spoof

    lookup = {(s.casefold(), k.casefold()): v for (s, k), v in wanted.items()}
    sec = None
    seen = set()
    for i, line in enumerate(lines):
        m = re.match(r"^\s*\[([^\]]+)\]", line.lstrip("\ufeff"))
        if m:
            sec = m.group(1).casefold()
            continue
        e = re.match(r"^\s*([A-Za-z0-9_]+)\s*=", line)
        if e and sec:
            k = (sec, e.group(1).casefold())
            if k in lookup and k not in seen:
                lines[i] = "%s=%s" % (e.group(1), lookup[k])
                seen.add(k)

    # Ensure [Spoofing] keys exist even if a stripped/custom INI omitted them.
    if spoof:
        missing = [
            key for key in ("Dxgi", "Vulkan", "VulkanExtensionSpoofing")
            if ("spoofing", key.casefold()) not in seen
        ]
        if missing:
            start = next(
                (i for i, line in enumerate(lines)
                 if re.match(r"^\s*\[Spoofing\]\s*$", line.lstrip("\ufeff"), re.IGNORECASE)),
                None,
            )
            if start is None:
                if lines and lines[-1] != "":
                    lines.append("")
                lines.append("[Spoofing]")
                end = len(lines)
            else:
                end = next(
                    (i for i in range(start + 1, len(lines)) if re.match(r"^\s*\[", lines[i])),
                    len(lines),
                )
            lines[end:end] = ["%s=%s" % (key, spoof) for key in missing]

    ini_path.write_bytes(nl.join(lines).encode("utf-8", errors="surrogateescape"))


def _append_manifest(game: Path, entries) -> None:
    manifest = game / MANIFEST
    existing = set()
    if manifest.is_file():
        existing = {line.strip() for line in manifest.read_text(encoding="utf-8", errors="replace").splitlines() if line.strip()}
    add = [e for e in entries if e not in existing]
    if not add:
        return
    with open(manifest, "a", encoding="utf-8") as f:
        for e in add:
            f.write(e + "\n")


def _copy_tree_filtered(src: Path, dst: Path, skip_top=()) -> None:
    dst.mkdir(parents=True, exist_ok=True)
    skip = set(skip_top)
    for item in os.scandir(src):
        if item.name in skip:
            continue
        target = dst / item.name
        if item.is_dir(follow_symlinks=False):
            shutil.copytree(item.path, target, dirs_exist_ok=True)
        else:
            shutil.copy2(item.path, target)


def install_optiscaler_only(pkg: Package, game_dir: Path, log: Callable[[str, str], None]) -> int:
    """Install OptiScaler (without Mochizuki NR) into ``game_dir`` using the same manifest format as upstream ``install.sh``."""
    root = pkg.root
    if not root or not pkg.has_optiscaler:
        log("err", "OptiScaler payload is missing from this package.")
        return 1
    game = Path(game_dir)
    manifest = game / MANIFEST
    if manifest.is_file():
        log("out", "Found a previous installation, removing it first.")
        proc = subprocess.run(
            ["bash", str(pkg.install_sh), str(game), "remove"],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
        )
        for ln in (proc.stdout or "").splitlines():
            log("out", ln)
        for ln in (proc.stderr or "").splitlines():
            log("err", ln)
        if proc.returncode != 0:
            return proc.returncode

    rel = root / "optiscaler" / "release"
    entries = [MANIFEST]

    def put_file(src: Path, dst_rel: str) -> None:
        dst = game / dst_rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        entries.append(dst_rel)

    put_file(rel / "OptiScaler.dll", "dxgi.dll")
    put_file(rel / "OptiScaler.ini", "OptiScaler.ini")
    shutil.copytree(rel / "OptiScaler", game / "OptiScaler", dirs_exist_ok=True)
    entries.append("OptiScaler/")
    if (rel / "Licenses").is_dir():
        shutil.copytree(rel / "Licenses", game / "OptiScaler" / "Licenses", dirs_exist_ok=True)
    opti_lic = root / "optiscaler" / "OptiScaler_LICENSE.txt"
    if opti_lic.is_file():
        (game / "OptiScaler" / "Licenses").mkdir(parents=True, exist_ok=True)
        shutil.copy2(opti_lic, game / "OptiScaler" / "Licenses" / "OptiScaler_LICENSE.txt")
    (game / "OptiScaler" / "plugins").mkdir(parents=True, exist_ok=True)

    if (game / "vulkan-1.dll").exists():
        log("out", "Note: the game folder has a vulkan-1.dll of its own; it is left as it is.")
    elif (root / "optiscaler" / "vulkan-1.dll").is_file():
        put_file(root / "optiscaler" / "vulkan-1.dll", "vulkan-1.dll")

    patch_ini = root / "optiscaler" / "patch_ini.py"
    if patch_ini.is_file():
        proc = subprocess.run([sys.executable, str(patch_ini), str(game)],
                              stdin=subprocess.DEVNULL, capture_output=True, text=True)
        for ln in (proc.stdout or "").splitlines():
            log("out", ln)
        for ln in (proc.stderr or "").splitlines():
            log("err", ln)
        if proc.returncode != 0:
            return proc.returncode

    entries.append("OptiScaler.log")
    manifest.write_text("\n".join(entries) + "\n", encoding="utf-8")
    log("out", "Installed the optiscaler route into: %s" % game)
    log("out", 'Steam launch options: WINEDLLOVERRIDES="dxgi=n,b" %command%')
    return 0


def apply_optiscaler_addons(
    pkg: Package,
    game_dir: Path,
    want_nr: bool,
    want_openrr: bool,
    openrr_spoof: str,
    want_optipatcher: bool,
    dlssd_path: Optional[str],
    log: Callable[[str, str], None],
) -> int:
    """Overlay OpenRR and/or OptiPatcher onto an installed OptiScaler game folder."""
    root = pkg.root
    if not root:
        log("err", "Package root not found.")
        return 1
    game = Path(game_dir)
    plugins = game / "OptiScaler" / "plugins"
    if not plugins.is_dir():
        log("err", "OptiScaler/plugins not found in %s." % game)
        return 1

    if want_openrr:
        if not pkg.has_openrr:
            log("err", "OpenRR is not bundled in this package (x86_64 only).")
            return 1
        shutil.copy2(root / "optiscaler" / "asi" / "openrr-amd.asi", plugins / "openrr-amd.asi")
        host_dll = root / "optiscaler" / "asi" / "openrr-ngx-host.dll"
        if host_dll.is_file():
            shutil.copy2(host_dll, plugins / "openrr-ngx-host.dll")
        _copy_tree_filtered(root / "openrr-amd", game / "openrr-amd", skip_top=("aco",))
        shutil.rmtree(game / "openrr-amd" / "aco", ignore_errors=True)
        ini_src = root / "openrr-amd.ini"
        if ini_src.is_file():
            shutil.copy2(ini_src, game / "openrr-amd.ini")

        # Resolve nvngx_dlssd.dll: user-provided (cached in package) -> package cache -> game folder
        pkg_cached_dlssd = root / "openrr-amd" / "nvngx_dlssd.dll"
        dlssd_installed = False
        if dlssd_path:
            try:
                extract_dlssd(Path(dlssd_path), pkg_cached_dlssd)
                shutil.copy2(pkg_cached_dlssd, game / "openrr-amd" / "nvngx_dlssd.dll")
                dlssd_installed = True
                log("out", "Cached nvngx_dlssd.dll into package and copied to openrr-amd/.")
            except ValueError as e:
                log("err", str(e))
                return 1
        else:
            for cand in (pkg_cached_dlssd, root / "nvngx_dlssd.dll", game / "nvngx_dlssd.dll"):
                if cand.is_file() and is_pe64_file(cand):
                    shutil.copy2(cand, game / "openrr-amd" / "nvngx_dlssd.dll")
                    dlssd_installed = True
                    log("out", "Copied nvngx_dlssd.dll to openrr-amd/ (from %s)." % cand)
                    break
        if not dlssd_installed:
            log("out", "Note: nvngx_dlssd.dll (310.9.1.0) was not found; place or import it into openrr-amd/ for Ray Reconstruction.")

        _append_manifest(game, OPENRR_MANIFEST_ENTRIES)
        log("out", "OpenRR %s installed (spoof: %s)." % (pkg.openrr_version or "0.1.7plus3", "auto" if openrr_spoof == "auto" else "full"))

    if want_optipatcher:
        if not pkg.has_optipatcher:
            log("err", "OptiPatcher.asi is not bundled in this package (x86_64 only).")
            return 1
        shutil.copy2(root / "optiscaler" / "asi" / "OptiPatcher.asi", plugins / "OptiPatcher.asi")
        log("out", "OptiPatcher.asi installed into OptiScaler/plugins/.")

    ini_path = game / "OptiScaler.ini"
    if ini_path.is_file():
        patch_optiscaler_ini(
            ini_path,
            want_nr=want_nr,
            want_openrr=want_openrr,
            openrr_spoof=openrr_spoof,
            prior_openrr=False,
        )
    return 0
