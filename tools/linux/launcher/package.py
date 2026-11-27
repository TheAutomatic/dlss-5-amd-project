"""The OptiScaler NR Linux package the launcher belongs to (or was pointed at).

The package root is the folder holding Mochizuki's upstream ``install.sh`` (shipped unmodified).
Everything the launcher knows about the payload - bitness, available routes, cached model - is
derived from that folder so that the same launcher code serves the x86_64 and the i686 package.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import threading
from pathlib import Path

# Fallback values; the real ones are read from the package's install.sh so that the installer
# stays the single source of truth.
MODEL_SHA256 = "2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd"  # CC_SILVER_AARDWOLD.bin
SMALL_SHA256 = "7e966e2639dbec6596aa0bac3b8761137d2c40ca8cb4aca30df7ca10043c6e33"  # CC_SMALL_FP8.bin

ROUTES = ("optiscaler", "reshade-feeder", "reshade-feeder-vulkan", "reshade-feeder-dx9")
MODELS = ("CC_SMALL_FP8", "CC_SMALL_FP8_OPTIMIZED", "CC_SILVER_AARDWOLD", "CC_SILVER_AARDWOLD_OPTIMIZED")

_hash_cache = {}
_hash_lock = threading.Lock()


def sha256_file(path: Path) -> str | None:
    """SHA-256 of a file, cached by (path, size, mtime) - model files are tens of MB."""
    try:
        st = path.stat()
    except OSError:
        return None
    key = (str(path), st.st_size, st.st_mtime_ns)
    with _hash_lock:
        if key in _hash_cache:
            return _hash_cache[key]
    h = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
    except OSError:
        return None
    digest = h.hexdigest()
    with _hash_lock:
        _hash_cache[key] = digest
    return digest


def is_package_root(path: Path) -> bool:
    return (path / "install.sh").is_file() and (path / "dlssnr-amd").is_dir()


def find_package(explicit: str | None = None) -> Path | None:
    """Locate the package root: --package, $OPTISCALER_NR_PACKAGE, then the launcher's parent."""
    candidates = []
    if explicit:
        candidates.append(Path(explicit).expanduser())
    env = os.environ.get("OPTISCALER_NR_PACKAGE")
    if env:
        candidates.append(Path(env).expanduser())
    candidates.append(Path(__file__).resolve().parent.parent)
    for c in candidates:
        try:
            c = c.resolve()
        except OSError:
            continue
        if is_package_root(c):
            return c
    return None


class Package:
    def __init__(self, root: Path | None):
        self.root = Path(root) if root else None
        self.meta = {}
        if self.root:
            try:
                self.meta = json.loads((self.root / "OptiScaler-NR-package.json").read_text(encoding="utf-8"))
            except (OSError, ValueError):
                self.meta = {}
        self.model_sha256, self.small_sha256 = self._read_hashes()

    # -- static facts ---------------------------------------------------------------------
    @property
    def ok(self) -> bool:
        return self.root is not None and is_package_root(self.root)

    @property
    def install_sh(self) -> Path | None:
        return self.root / "install.sh" if self.root else None

    @property
    def bits(self) -> int:
        """Same rule as install.sh: the 32-bit package has the 32-bit ReShade add-on."""
        if self.root and (self.root / "reshade" / "dlssnr_amd.addon32").is_file():
            return 32
        return 64

    @property
    def arch(self) -> str:
        return self.meta.get("arch") or ("i686" if self.bits == 32 else "x86_64")

    @property
    def has_optiscaler(self) -> bool:
        return bool(self.root and (self.root / "optiscaler").is_dir())

    @property
    def has_openrr(self) -> bool:
        return bool(
            self.root
            and (self.root / "optiscaler" / "asi" / "openrr-amd.asi").is_file()
            and (self.root / "openrr-amd").is_dir()
        )

    @property
    def openrr_version(self) -> str:
        if self.meta.get("openrrVersion"):
            return str(self.meta["openrrVersion"])
        return "0.1.7.1plus2" if self.has_openrr else ""

    @property
    def has_optipatcher(self) -> bool:
        return bool(self.root and (self.root / "optiscaler" / "asi" / "OptiPatcher.asi").is_file())

    @property
    def version(self) -> str:
        if self.meta.get("version"):
            return str(self.meta["version"])
        candidates = [self.root / "VERSION"] if self.root else []
        here = Path(__file__).resolve()
        if len(here.parents) > 3:  # running from the repo: tools/linux/launcher -> repo/VERSION
            candidates.append(here.parents[3] / "VERSION")
        for p in candidates:
            try:
                v = p.read_text(encoding="utf-8").strip()
                if v:
                    return v
            except OSError:
                pass
        return "dev"

    @property
    def upstream_version(self) -> str:
        return str(self.meta.get("upstreamVersion") or "")

    def routes(self) -> dict:
        """Route -> availability (+ reason when unavailable)."""
        out = {}
        for r in ROUTES:
            if r == "optiscaler" and not self.has_optiscaler:
                out[r] = {"available": False, "reason": "only64"}
            else:
                out[r] = {"available": self.ok, "reason": None if self.ok else "nopackage"}
        return out

    def _read_hashes(self):
        model, small = MODEL_SHA256, SMALL_SHA256
        if self.root:
            try:
                text = (self.root / "install.sh").read_text(encoding="utf-8", errors="replace")
                m = re.search(r"^model_sha256=([0-9a-f]{64})\s*$", text, re.M)
                s = re.search(r"^small(?:_fp8)?_sha256=([0-9a-f]{64})\s*$", text, re.M)
                model = m.group(1) if m else model
                small = s.group(1) if s else small
            except OSError:
                pass
        return model, small

    # -- model cache -----------------------------------------------------------------------
    def model_status(self) -> dict:
        """State of the models kept in the package's dlssnr-amd/ (put there by a --dll install).

        Each of ``aardwold`` / ``small`` is "ok", "missing" or "bad". ``ready`` means an install can
        run without --dll (install.sh only strictly needs CC_SILVER_AARDWOLD; without CC_SMALL_FP8 the
        CC_SMALL_FP8 networks are greyed out in game).
        """
        def check(name, expected):
            if not self.root:
                return "missing"
            p = self.root / "dlssnr-amd" / name
            if not p.is_file():
                return "missing"
            return "ok" if sha256_file(p) == expected else "bad"

        aard = check("CC_SILVER_AARDWOLD.bin", self.model_sha256)
        small = check("CC_SMALL_FP8.bin", self.small_sha256)
        return {
            "aardwold": aard,
            "small": small,
            "ready": aard == "ok" and small != "bad",
            "complete": aard == "ok" and small == "ok",
        }

    def dlssd_status(self) -> dict:
        """State of user-supplied nvngx_dlssd.dll cached in this package for OpenRR."""
        if not self.root or not self.has_openrr:
            return {"cached": False, "path": None}
        for cand in (self.root / "openrr-amd" / "nvngx_dlssd.dll", self.root / "nvngx_dlssd.dll"):
            if cand.is_file():
                try:
                    with open(cand, "rb") as f:
                        head = f.read(2)
                    if head == b"MZ":
                        return {"cached": True, "path": str(cand)}
                except OSError:
                    pass
        return {"cached": False, "path": None}

    def info(self) -> dict:
        return {
            "ok": self.ok,
            "root": str(self.root) if self.root else None,
            "arch": self.arch,
            "bits": self.bits,
            "version": self.version,
            "upstreamVersion": self.upstream_version,
            "openrr": {"available": self.has_openrr, "version": self.openrr_version},
            "optipatcher": self.has_optipatcher,
            "routes": self.routes(),
            "models": list(MODELS),
        }
