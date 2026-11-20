"""Persistent launcher state: settings, manual library entries, folder overrides and installs.

Stored as JSON in ``$XDG_CONFIG_HOME/optiscaler-nr/launcher.json``. Writes are atomic
(temp file + rename) and serialised by a lock because the HTTP server is threaded.
"""
from __future__ import annotations

import copy
import json
import os
import tempfile
import threading
from pathlib import Path

from . import APP_ID


def config_dir() -> Path:
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return Path(base) / APP_ID


def cache_dir() -> Path:
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    return Path(base) / APP_ID


def state_file() -> Path:
    return config_dir() / "launcher.json"


DEFAULTS = {
    "schema": 1,
    # lang: "zh" | "en" | None (None = follow $LANG); theme: "light" | "dark" | "system"
    "settings": {"lang": None, "theme": "system"},
    # manually added game folders (absolute paths)
    "manual": [],
    # game id -> folder chosen by the user instead of the auto-detected exe folder
    "overrides": {},
    # game folder -> details of the last successful install made by this launcher
    "installs": {},
    # game id -> last form choices (route/model/style/lumenite)
    "choices": {},
}


class State:
    def __init__(self, path: Path | None = None):
        self.path = Path(path) if path else state_file()
        self._lock = threading.RLock()
        self.data = copy.deepcopy(DEFAULTS)
        self.load()

    def load(self) -> None:
        with self._lock:
            try:
                raw = json.loads(self.path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                return
            if not isinstance(raw, dict):
                return
            for key, default in DEFAULTS.items():
                value = raw.get(key, copy.deepcopy(default))
                if isinstance(default, dict) and isinstance(value, dict):
                    merged = copy.deepcopy(default)
                    merged.update(value)
                    value = merged
                if not isinstance(value, type(default)):
                    value = copy.deepcopy(default)
                self.data[key] = value

    def save(self) -> None:
        with self._lock:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            fd, tmp = tempfile.mkstemp(prefix=".launcher-", suffix=".json", dir=str(self.path.parent))
            try:
                with os.fdopen(fd, "w", encoding="utf-8") as f:
                    json.dump(self.data, f, ensure_ascii=False, indent=2, sort_keys=True)
                    f.write("\n")
                os.replace(tmp, self.path)
            except BaseException:
                try:
                    os.unlink(tmp)
                except OSError:
                    pass
                raise

    # -- helpers used by the server -------------------------------------------------------
    def snapshot(self) -> dict:
        with self._lock:
            return copy.deepcopy(self.data)

    def update(self, fn) -> None:
        """Run ``fn(data)`` under the lock, then persist."""
        with self._lock:
            fn(self.data)
            self.save()
