"""Local HTTP server: the single-page UI plus a small JSON API.

Security model (the server can modify game folders, so it must only obey our own UI):
- binds to 127.0.0.1 only, on a random port by default;
- a random per-session token is required on every /api/ call (header ``X-NR-Token`` or ``t``
  query parameter for <img>/EventSource URLs) and compared in constant time;
- the Host header must be 127.0.0.1:<port> or localhost:<port> (DNS-rebinding protection) and a
  present Origin header must match it; POST bodies must be JSON (no simple cross-site form posts);
- strict Content-Security-Policy, no third-party content at all.
The server exits by itself when the UI is gone (no event stream and no heartbeat for ~45 s).
"""
from __future__ import annotations

import hmac
import json
import mimetypes
import os
import secrets
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from . import desktop, library
from .jobs import JobRunner, build_command, path_ok_for_dll
from .package import MODELS, Package
from .state import State

WEB_DIR = Path(__file__).resolve().parent / "web"
STATIC = {"/app.js": "app.js", "/app.css": "app.css", "/icon.svg": "icon.svg", "/favicon.svg": "icon.svg"}
IDLE_TIMEOUT = 45.0       # seconds without event stream / heartbeat before exiting
STARTUP_GRACE = 120.0     # seconds to wait for the window to connect the first time
MAX_BODY = 1 << 20

CSP = ("default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; "
       "connect-src 'self'; font-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")


class ApiError(Exception):
    def __init__(self, status: int, code: str, message: str):
        super().__init__(message)
        self.status, self.code, self.message = status, code, message


def default_lang() -> str:
    for var in ("LC_ALL", "LC_MESSAGES", "LANGUAGE", "LANG"):
        v = os.environ.get(var, "")
        if v:
            return "zh" if v.lower().startswith("zh") else "en"
    return "en"


class App:
    """Everything the request handlers share."""

    def __init__(self, pkg: Package, state: State, home: Path | None = None, wait_for_first_contact: bool = False,
                 entry_script: Path | None = None):
        self.pkg = pkg
        self.state = state
        self.home = Path(home) if home else Path.home()
        self.token = secrets.token_urlsafe(24)
        self.jobs = JobRunner()
        self.port = 0
        self.entry_script = entry_script
        self._games = None
        self._games_lock = threading.Lock()
        self.last_seen = time.monotonic()
        self.ever_connected = False
        self.wait_for_first_contact = wait_for_first_contact
        self.streams = 0
        self._streams_lock = threading.Lock()
        self.quit = threading.Event()
        self.httpd = None

    # -- library ----------------------------------------------------------------------------
    def games(self, refresh: bool = False) -> list:
        with self._games_lock:
            if self._games is None or refresh:
                snap = self.state.snapshot()
                self._games = library.scan_all(self.home, snap["manual"], snap["overrides"])
            return self._games

    def game(self, gid: str) -> dict:
        for g in self.games():
            if g["id"] == gid:
                return g
        raise ApiError(404, "unknown_game", "unknown game: %s" % gid)

    def refresh_game(self, gid: str) -> None:
        with self._games_lock:
            for g in self._games or []:
                if g["id"] == gid:
                    library.enrich(g, self.state.snapshot()["overrides"].get(gid))

    def public_game(self, g: dict) -> dict:
        snap = self.state.snapshot()
        keys = ("id", "source", "store", "appid", "name", "folder", "installDir", "exe", "exeBits", "folderBits",
                "status", "missing", "manual", "override")
        out = {k: g.get(k) for k in keys}
        out["art"] = {k: True for k in (g.get("art") or {})}
        out["record"] = snap["installs"].get(g["folder"]) if g["status"]["installed"] else None
        out["choices"] = snap["choices"].get(g["id"])
        out["bitsMismatch"] = bool(g.get("exeBits") and g["exeBits"] != self.pkg.bits)
        return out

    # -- liveness -----------------------------------------------------------------------------
    def touch(self) -> None:
        self.last_seen = time.monotonic()
        self.ever_connected = True

    def stream_opened(self) -> None:
        with self._streams_lock:
            self.streams += 1
        self.touch()

    def stream_closed(self) -> None:
        with self._streams_lock:
            self.streams -= 1
        self.last_seen = time.monotonic()

    def watchdog(self) -> None:
        while not self.quit.wait(2.0):
            if self.jobs.busy() or self.streams > 0:
                self.last_seen = time.monotonic()
                continue
            idle = time.monotonic() - self.last_seen
            if self.ever_connected:
                limit = IDLE_TIMEOUT
            elif self.wait_for_first_contact:
                continue
            else:
                limit = STARTUP_GRACE
            if idle > limit:
                print("The launcher window is gone; exiting.", flush=True)
                self.shutdown()

    def shutdown(self) -> None:
        if self.quit.is_set():
            return
        self.quit.set()
        if self.httpd:
            threading.Thread(target=self.httpd.shutdown, daemon=True).start()

    # -- jobs -----------------------------------------------------------------------------------
    def start_install(self, body: dict):
        g = self.game(str(body.get("id") or ""))
        folder = g["folder"]
        if not self.pkg.ok:
            raise ApiError(400, "no_package", "The package payload (install.sh) was not found.")
        if not Path(folder).is_dir():
            raise ApiError(400, "folder_missing", "The game folder does not exist: %s" % folder)
        route = body.get("route")
        routes = self.pkg.routes()
        if route not in routes:
            raise ApiError(400, "bad_route", "unknown route: %s" % route)
        if not routes[route]["available"]:
            raise ApiError(400, "route_unavailable",
                           "The OptiScaler route is only in the 64-bit (x86_64) package." if route == "optiscaler"
                           else "This route is not available.")
        model = body.get("model") or None
        if model is not None and model not in MODELS:
            raise ApiError(400, "bad_model", "unknown model: %s" % model)
        style = "wilsjo2" if body.get("style") == "wilsjo2" else "renodx"
        lumenite = bool(body.get("lumenite")) and route != "optiscaler"
        dll = (body.get("dll") or "").strip() or None
        if dll:
            dll = os.path.expanduser(dll)
            if not path_ok_for_dll(dll):
                raise ApiError(400, "dll_invalid", "Not an nvngx_dlssnr.dll or .zip file: %s" % dll)
        elif not self.pkg.model_status()["ready"] and not body.get("skipModelCheck"):
            raise ApiError(400, "model_required",
                           "The package has no model yet: choose NVIDIA's nvngx_dlssnr.dll (310.9.16 or 310.8.0) "
                           "or a zip containing it. It is needed only the first time.")
        cmd = build_command(self.pkg, folder, "install", route, model, style, lumenite, dll)
        meta = {"gameId": g["id"], "name": g["name"], "folder": folder, "route": route, "model": model or "CC_SMALL_FP8",
                "style": style, "lumenite": lumenite, "withDll": bool(dll)}
        choices = {"route": route, "model": model or "CC_SMALL_FP8", "style": style, "lumenite": lumenite}
        self.state.update(lambda d: d["choices"].__setitem__(g["id"], choices))

        def done(job):
            if job.exit_code == 0:
                rec = {"route": route, "model": meta["model"], "style": style, "lumenite": lumenite,
                       "launch": job.result.get("launch"), "launchAlt": job.result.get("launchAlt"),
                       "gameId": g["id"], "name": g["name"], "time": int(time.time()),
                       "package": str(self.pkg.root), "arch": self.pkg.arch}
                self.state.update(lambda d: d["installs"].__setitem__(folder, rec))
            self.refresh_game(g["id"])
        return self._start("install", cmd, meta, done)

    def start_simple(self, kind: str, body: dict):
        g = self.game(str(body.get("id") or ""))
        folder = g["folder"]
        if not self.pkg.ok:
            raise ApiError(400, "no_package", "The package payload (install.sh) was not found.")
        if not Path(folder).is_dir():
            raise ApiError(400, "folder_missing", "The game folder does not exist: %s" % folder)
        cmd = build_command(self.pkg, folder, kind)
        meta = {"gameId": g["id"], "name": g["name"], "folder": folder}

        def done(job):
            if kind == "remove" and job.exit_code == 0:
                self.state.update(lambda d: d["installs"].pop(folder, None))
            self.refresh_game(g["id"])
        return self._start(kind, cmd, meta, done)

    def _start(self, kind, cmd, meta, done):
        try:
            job = self.jobs.start(kind, cmd, meta, cwd=str(self.pkg.root), on_done=done)
        except RuntimeError:
            raise ApiError(409, "busy", "Another operation is still running.")
        return {"job": job.to_dict()}

    # -- file browser ---------------------------------------------------------------------------
    def places(self) -> list:
        out = [{"key": "home", "path": str(self.home)}]
        for name in ("Downloads", "Desktop", "Documents"):
            p = self.home / name
            if p.is_dir():
                out.append({"key": name.lower(), "path": str(p)})
        seen = set()
        for root in library.steam_roots(self.home):
            for lib in library.steam_libraries(root):
                common = lib / "steamapps" / "common"
                if common.is_dir() and str(common) not in seen:
                    seen.add(str(common))
                    out.append({"key": "steam", "path": str(common)})
        user = os.environ.get("USER") or self.home.name
        for base in (Path("/run/media") / user, Path("/media") / user, Path("/mnt")):
            try:
                for e in sorted(os.scandir(base), key=lambda e: e.name):
                    if e.is_dir():
                        out.append({"key": "drive", "path": e.path})
            except OSError:
                pass
        return out

    def browse(self, path: str | None, mode: str, hidden: bool) -> dict:
        p = Path(os.path.expanduser(path)) if path else self.home
        if p.is_file():
            p = p.parent
        while not p.is_dir() and p != p.parent:
            p = p.parent
        entries = []
        error = None
        try:
            items = list(os.scandir(p))
        except OSError as e:
            items, error = [], str(e.strerror or e)
        for e in items:
            if not hidden and e.name.startswith("."):
                continue
            try:
                is_dir = e.is_dir()
            except OSError:
                continue
            if is_dir:
                entries.append({"name": e.name, "dir": True})
            elif mode == "model" and e.name.lower().endswith((".dll", ".zip")):
                try:
                    size = e.stat().st_size
                except OSError:
                    size = None
                entries.append({"name": e.name, "dir": False, "size": size})
            elif mode == "dir" and e.name.lower().endswith(".exe"):
                entries.append({"name": e.name, "dir": False, "exe": True})
        entries.sort(key=lambda x: (not x["dir"], x["name"].casefold()))
        return {"path": str(p), "parent": str(p.parent) if p != p.parent else None,
                "entries": entries[:3000], "truncated": len(entries) > 3000, "error": error}

    # -- info -------------------------------------------------------------------------------------
    def info(self) -> dict:
        snap = self.state.snapshot()
        d_path, _ = desktop.desktop_paths()
        return {
            "package": self.pkg.info(),
            "model": self.pkg.model_status() if self.pkg.ok else {"aardwold": "missing", "small": "missing",
                                                                    "ready": False, "complete": False},
            "settings": snap["settings"],
            "defaultLang": default_lang(),
            "desktopInstalled": d_path.exists(),
            "busy": self.jobs.busy(),
            "home": str(self.home),
            "stateFile": str(self.state.path),
        }


class Handler(BaseHTTPRequestHandler):
    server_version = "OptiScalerNR"
    sys_version = ""
    protocol_version = "HTTP/1.1"
    app: App = None  # set on the subclass created by make_server

    def log_message(self, fmt, *args):  # quiet unless debugging
        if os.environ.get("OPTISCALER_NR_DEBUG"):
            super().log_message(fmt, *args)

    # -- plumbing ---------------------------------------------------------------------------------
    def _host_ok(self) -> bool:
        host = (self.headers.get("Host") or "").strip().lower()
        allowed = {"127.0.0.1:%d" % self.app.port, "localhost:%d" % self.app.port}
        if host not in allowed:
            return False
        origin = self.headers.get("Origin")
        if origin and origin.lower() not in {"http://" + a for a in allowed}:
            return False
        return True

    def _token_ok(self, query: dict) -> bool:
        tok = self.headers.get("X-NR-Token") or (query.get("t") or [""])[0]
        return bool(tok) and hmac.compare_digest(tok.encode(), self.app.token.encode())

    def _headers(self, status: int, ctype: str, length: int | None, extra: dict | None = None):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        if length is not None:
            self.send_header("Content-Length", str(length))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Content-Security-Policy", CSP)
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()

    def _send(self, status: int, body: bytes, ctype: str, extra: dict | None = None):
        self._headers(status, ctype, len(body), extra)
        if self.command != "HEAD":
            self.wfile.write(body)

    def _json(self, obj, status: int = 200):
        self._send(status, json.dumps(obj, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8")

    def _error(self, status: int, code: str, message: str):
        self._json({"ok": False, "error": code, "message": message}, status)

    def _body(self) -> dict:
        ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
        if ctype != "application/json":
            raise ApiError(415, "bad_content_type", "JSON body required")
        length = int(self.headers.get("Content-Length") or 0)
        if length < 0 or length > MAX_BODY:
            raise ApiError(413, "too_large", "body too large")
        raw = self.rfile.read(length) if length else b"{}"
        try:
            data = json.loads(raw.decode("utf-8") or "{}")
        except ValueError:
            raise ApiError(400, "bad_json", "invalid JSON")
        if not isinstance(data, dict):
            raise ApiError(400, "bad_json", "JSON object expected")
        return data

    # -- dispatch ----------------------------------------------------------------------------------
    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        self._dispatch("GET")

    def do_POST(self):
        self._dispatch("POST")

    def _dispatch(self, method: str):
        url = urlparse(self.path)
        query = parse_qs(url.query)
        if not self._host_ok():
            return self._error(403, "bad_host", "forbidden")
        try:
            if not url.path.startswith("/api/"):
                if method != "GET":
                    return self._error(405, "method", "method not allowed")
                return self._static(url.path, query)
            if not self._token_ok(query):
                return self._error(403, "bad_token", "missing or invalid session token")
            if url.path in ("/api/heartbeat", "/api/info"):
                self.app.touch()
            fn = ROUTES.get((method, url.path))
            if fn is None:
                return self._error(404, "not_found", "no such endpoint")
            body = self._body() if method == "POST" else {}
            result = fn(self, query, body)
            if result is not None:
                self._json(result)
        except ApiError as e:
            self._error(e.status, e.code, e.message)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:  # report, never crash the server thread silently
            traceback.print_exc()
            try:
                self._error(500, "internal", "%s: %s" % (type(e).__name__, e))
            except OSError:
                pass

    def _static(self, path: str, query: dict):
        if path in ("/", "/index.html"):
            if not self._token_ok(query):
                body = ("<!doctype html><meta charset=utf-8><title>OptiScaler NR</title>"
                        "<p style='font-family:sans-serif'>Session token missing. Start the launcher with "
                        "OptiScalerNR-Launcher.sh.</p>").encode()
                return self._send(403, body, "text/html; charset=utf-8")
            name = "index.html"
        elif path in STATIC:
            name = STATIC[path]
        else:
            return self._error(404, "not_found", "not found")
        f = WEB_DIR / name
        try:
            data = f.read_bytes()
        except OSError:
            return self._error(404, "not_found", "not found")
        ctype = mimetypes.guess_type(name)[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript", "image/svg+xml"):
            ctype += "; charset=utf-8"
        self._send(200, data, ctype)

    # -- endpoints -----------------------------------------------------------------------------------
    def api_info(self, q, b):
        return self.app.info()

    def api_games(self, q, b):
        refresh = (q.get("refresh") or ["0"])[0] == "1"
        return {"games": [self.app.public_game(g) for g in self.app.games(refresh)]}

    def api_game(self, q, b):
        gid = (q.get("id") or [""])[0]
        if (q.get("refresh") or ["0"])[0] == "1":
            self.app.refresh_game(gid)
        return {"game": self.app.public_game(self.app.game(gid))}

    def api_art(self, q, b):
        g = self.app.game((q.get("id") or [""])[0])
        kind = (q.get("kind") or ["thumb"])[0]
        art = g.get("art") or {}
        path = art.get(kind) or art.get("hero") or art.get("thumb")
        if not path:
            raise ApiError(404, "no_art", "no art")
        try:
            data = Path(path).read_bytes()
        except OSError:
            raise ApiError(404, "no_art", "no art")
        ctype = mimetypes.guess_type(path)[0] or "image/jpeg"
        self._send(200, data, ctype)
        return None

    def api_settings(self, q, b):
        def apply(d):
            s = d["settings"]
            if "lang" in b and b["lang"] in ("zh", "en", None):
                s["lang"] = b["lang"]
            if b.get("theme") in ("light", "dark", "system"):
                s["theme"] = b["theme"]
        self.app.state.update(apply)
        return {"ok": True, "settings": self.app.state.snapshot()["settings"]}

    def api_library_add(self, q, b):
        raw = os.path.expanduser(str(b.get("path") or "").strip())
        if not raw or not Path(raw).is_dir():
            raise ApiError(400, "not_a_folder", "Not a folder: %s" % raw)
        path = str(Path(raw).resolve())
        self.app.state.update(lambda d: path in d["manual"] or d["manual"].append(path))
        games = self.app.games(refresh=True)
        gid = library.manual_game(path)["id"]
        match = next((g for g in games if g["id"] == gid), None)
        if match is None:  # the folder already belongs to a scanned game
            folder, _ = library.resolve_game_folder(path)
            match = next((g for g in games if g["folder"] == str(folder)), None)
        return {"ok": True, "id": match["id"] if match else None}

    def api_library_remove(self, q, b):
        g = self.app.game(str(b.get("id") or ""))
        if g.get("source") != "manual":
            raise ApiError(400, "not_manual", "Only manually added games can be removed from the list.")
        self.app.state.update(lambda d: d.__setitem__("manual", [m for m in d["manual"] if m != g["installDir"]]))
        self.app.games(refresh=True)
        return {"ok": True}

    def api_library_override(self, q, b):
        gid = str(b.get("id") or "")
        self.app.game(gid)
        folder = b.get("folder")
        if folder:
            folder = str(Path(os.path.expanduser(str(folder))).resolve())
            if not Path(folder).is_dir():
                raise ApiError(400, "not_a_folder", "Not a folder: %s" % folder)
            self.app.state.update(lambda d: d["overrides"].__setitem__(gid, folder))
        else:
            self.app.state.update(lambda d: d["overrides"].pop(gid, None))
        self.app.games(refresh=True)
        return {"ok": True, "game": self.app.public_game(self.app.game(gid))}

    def api_browse(self, q, b):
        mode = (q.get("mode") or ["dir"])[0]
        hidden = (q.get("hidden") or ["0"])[0] == "1"
        res = self.app.browse((q.get("path") or [""])[0] or None, "model" if mode == "model" else "dir", hidden)
        res["places"] = self.app.places()
        return res

    def api_install(self, q, b):
        return self.app.start_install(b)

    def api_uninstall(self, q, b):
        return self.app.start_simple("remove", b)

    def api_logs(self, q, b):
        return self.app.start_simple("logs", b)

    def api_job(self, q, b):
        job = self.app.jobs.get((q.get("id") or [""])[0]) if q.get("id") else self.app.jobs.latest()
        if job is None:
            return {"job": None}
        since = int((q.get("since") or ["0"])[0] or 0)
        return {"job": job.to_dict(max(0, since))}

    def api_open(self, q, b):
        target = b.get("target")
        if target == "package":
            path = self.app.pkg.root
        elif target == "logs":
            p = Path(str(b.get("path") or ""))
            if not (p.is_file() and p.name.startswith("dlssnr-amd-logs-")):
                raise ApiError(400, "bad_path", "not a log archive")
            path = p.parent
        else:
            path = Path(self.app.game(str(b.get("id") or ""))["folder"])
        if not path or not Path(path).is_dir():
            raise ApiError(400, "bad_path", "folder not found")
        return {"ok": desktop.open_path(str(path))}

    def api_desktop(self, q, b):
        if b.get("action") == "remove":
            return {"ok": True, "removed": desktop.remove_desktop()}
        entry = self.app.entry_script
        if not entry or not Path(entry).is_file():
            raise ApiError(400, "no_entry", "OptiScalerNR-Launcher.sh not found next to the launcher")
        path = desktop.install_desktop(Path(entry), WEB_DIR / "icon.svg", self.app.pkg.version, self.app.pkg.arch)
        return {"ok": True, "path": str(path)}

    def api_heartbeat(self, q, b):
        return {"ok": True, "busy": self.app.jobs.busy()}

    def api_quit(self, q, b):
        if self.app.jobs.busy():
            raise ApiError(409, "busy", "An operation is still running.")
        self._json({"ok": True})
        self.app.shutdown()
        return None

    def api_events(self, q, b):
        """Server-sent events: the current job's output, live. Also the window's liveness signal."""
        app = self.app
        self._headers(200, "text/event-stream; charset=utf-8", None, {"Connection": "keep-alive"})
        self.close_connection = True
        app.stream_opened()

        def send(obj):
            self.wfile.write(("data: %s\n\n" % json.dumps(obj, ensure_ascii=False)).encode("utf-8"))
            self.wfile.flush()

        try:
            send({"type": "hello", "busy": app.jobs.busy()})
            job, sent, sent_state = None, 0, None
            last_write = time.monotonic()
            while not app.quit.is_set():
                cur = app.jobs.latest()
                if cur is not None and cur is not job:
                    job = cur
                    d = job.to_dict(0)
                    sent, sent_state = d["count"], d["state"]
                    send({"type": "job", "job": d})
                    last_write = time.monotonic()
                    continue
                if job is not None:
                    with job.cond:
                        if len(job.lines) == sent and job.state == sent_state:
                            job.cond.wait(0.5)
                    d = job.to_dict(sent)
                    if d["lines"]:
                        send({"type": "lines", "id": job.id, "since": sent, "lines": d["lines"]})
                        sent = d["count"]
                        last_write = time.monotonic()
                    if d["state"] != sent_state:
                        sent_state = d["state"]
                        d["lines"] = []
                        send({"type": "job", "job": d})
                        last_write = time.monotonic()
                else:
                    time.sleep(0.4)
                if time.monotonic() - last_write > 10:
                    self.wfile.write(b": ping\n\n")
                    self.wfile.flush()
                    last_write = time.monotonic()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            app.stream_closed()
        return None


ROUTES = {
    ("GET", "/api/info"): Handler.api_info,
    ("GET", "/api/games"): Handler.api_games,
    ("GET", "/api/game"): Handler.api_game,
    ("GET", "/api/art"): Handler.api_art,
    ("GET", "/api/browse"): Handler.api_browse,
    ("GET", "/api/job"): Handler.api_job,
    ("GET", "/api/events"): Handler.api_events,
    ("POST", "/api/settings"): Handler.api_settings,
    ("POST", "/api/library/add"): Handler.api_library_add,
    ("POST", "/api/library/remove"): Handler.api_library_remove,
    ("POST", "/api/library/override"): Handler.api_library_override,
    ("POST", "/api/install"): Handler.api_install,
    ("POST", "/api/uninstall"): Handler.api_uninstall,
    ("POST", "/api/logs"): Handler.api_logs,
    ("POST", "/api/open"): Handler.api_open,
    ("POST", "/api/desktop"): Handler.api_desktop,
    ("POST", "/api/heartbeat"): Handler.api_heartbeat,
    ("POST", "/api/quit"): Handler.api_quit,
}


class _Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def make_server(app: App, port: int = 0) -> _Server:
    handler = type("BoundHandler", (Handler,), {"app": app})
    httpd = _Server(("127.0.0.1", port), handler)
    app.port = httpd.server_address[1]
    app.httpd = httpd
    return httpd


def url_for(app: App, **extra) -> str:
    q = "&".join(["t=" + app.token] + ["%s=%s" % (k, v) for k, v in extra.items()])
    return "http://127.0.0.1:%d/?%s" % (app.port, q)
