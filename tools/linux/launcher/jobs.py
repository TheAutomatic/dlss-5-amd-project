"""Running the upstream installer (install.sh) as a background job and parsing its output.

install.sh is driven non-interactively: stdin is /dev/null (so it never prompts) and every
choice is passed as an explicit flag. stdout and stderr are streamed line by line into the job,
tagged by stream, so the UI can show a live console and surface the actual error message.
"""
from __future__ import annotations

import itertools
import os
import re
import subprocess
import threading
import time
from pathlib import Path

from .package import MODELS, ROUTES, Package

_ids = itertools.count(1)


def build_command(pkg: Package, folder: str, action: str, route: str | None = None, model: str | None = None,
                  style: str | None = None, lumenite: bool | None = None, dll: str | None = None) -> list:
    """argv for install.sh. ``action`` is install | remove | logs."""
    cmd = ["bash", str(pkg.install_sh), str(folder)]
    if action == "remove":
        return cmd + ["remove"]
    if action == "logs":
        return cmd + ["logs"]
    if route not in ROUTES:
        raise ValueError("unknown route: %s" % route)
    cmd.append(route)
    if model:
        if model not in MODELS:
            raise ValueError("unknown model: %s" % model)
        cmd += ["--model", model]
    cmd.append("--wilsjo2" if style == "wilsjo2" else "--renodx")
    if route != "optiscaler":
        cmd.append("--lumenite" if lumenite else "--no-lumenite")
    if dll:
        cmd += ["--dll", str(dll)]
    return cmd


_LAUNCH = re.compile(r"^Steam launch options:\s*(.+?)\s*$")
_ALT_HEAD = "use this line instead"
_LOGS = re.compile(r"^Logs packed:\s*(.+?)\s*$")
_INSTALLED = re.compile(r"^Installed the (\S+) route into: ")


def parse_output(lines) -> dict:
    """Pull the useful facts out of install.sh's output.

    ``lines`` is an iterable of (stream, text). Returns launch (main Steam launch options line),
    launchAlt (the int4 Vulkan-layer variant, if printed), logs (packed archive path), installed
    (route reported by the installer), removed, and error (stderr lines that are not notes).
    """
    out = {"launch": None, "launchAlt": None, "logs": None, "installed": None, "removed": False,
           "nothing": False, "error": None, "notes": []}
    errors = []
    expect_alt = False
    for stream, text in lines:
        t = text.rstrip()
        s = t.strip()
        if expect_alt and s:
            if "%command%" in s:
                out["launchAlt"] = s
            expect_alt = False
        m = _LAUNCH.match(s)
        if m:
            out["launch"] = m.group(1)
            continue
        if _ALT_HEAD in s and s.endswith(":"):
            expect_alt = True
            continue
        m = _LOGS.match(s)
        if m:
            out["logs"] = m.group(1)
        m = _INSTALLED.match(s)
        if m:
            out["installed"] = m.group(1)
        if s.startswith("Uninstalled from "):
            out["removed"] = True
        if s.endswith("not found, nothing to uninstall."):
            out["nothing"] = True
        if s.startswith("Note:"):
            out["notes"].append(s)
        elif stream == "err" and s:
            errors.append(s)
    if errors:
        out["error"] = "\n".join(errors[-4:])
    return out


class Job:
    def __init__(self, kind: str, cmd: list, meta: dict):
        self.id = next(_ids)
        self.kind = kind
        self.cmd = cmd
        self.meta = meta
        self.lines = []          # list of (stream, text)
        self.state = "running"   # running | done | failed
        self.exit_code = None
        self.result = {}
        self.started = time.time()
        self.ended = None
        self.cond = threading.Condition()
        self._proc = None

    def add(self, stream: str, text: str) -> None:
        with self.cond:
            self.lines.append((stream, text))
            self.cond.notify_all()

    def to_dict(self, since: int = 0) -> dict:
        with self.cond:
            return {
                "id": self.id,
                "kind": self.kind,
                "state": self.state,
                "exitCode": self.exit_code,
                "meta": self.meta,
                "result": self.result,
                "count": len(self.lines),
                "since": since,
                "lines": [{"s": s, "t": t} for s, t in self.lines[since:]],
                "started": self.started,
                "ended": self.ended,
            }


class JobRunner:
    """One job at a time (install.sh edits game folders and the package's model cache)."""

    def __init__(self):
        self._lock = threading.Lock()
        self.jobs = {}
        self.current = None
        self.listeners = []  # callables(job) notified on job end

    def busy(self) -> bool:
        with self._lock:
            return self.current is not None and self.current.state == "running"

    def get(self, job_id) -> Job | None:
        try:
            return self.jobs.get(int(job_id))
        except (TypeError, ValueError):
            return None

    def latest(self) -> Job | None:
        with self._lock:
            return self.current

    def start(self, kind: str, cmd: list, meta: dict, cwd: str | None = None, on_done=None) -> Job:
        with self._lock:
            if self.current is not None and self.current.state == "running":
                raise RuntimeError("busy")
            job = Job(kind, cmd, meta)
            self.jobs[job.id] = job
            self.current = job
        t = threading.Thread(target=self._run, args=(job, cwd, on_done), name="job-%d" % job.id, daemon=True)
        t.start()
        return job

    def _run(self, job: Job, cwd, on_done) -> None:
        env = dict(os.environ)
        env.setdefault("LC_ALL", "C.UTF-8")
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        job.add("sys", "$ " + " ".join(_quote(a) for a in job.cmd))
        try:
            proc = subprocess.Popen(job.cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    cwd=cwd, env=env, start_new_session=True)
        except OSError as e:
            job.add("err", "could not start the installer: %s" % e)
            self._finish(job, 127, on_done)
            return
        job._proc = proc

        def pump(stream, tag):
            for raw in iter(stream.readline, b""):
                job.add(tag, raw.decode("utf-8", "replace").rstrip("\r\n"))
            stream.close()

        readers = [threading.Thread(target=pump, args=(proc.stdout, "out"), daemon=True),
                   threading.Thread(target=pump, args=(proc.stderr, "err"), daemon=True)]
        for r in readers:
            r.start()
        code = proc.wait()
        for r in readers:
            r.join(timeout=5)
        self._finish(job, code, on_done)

    def _finish(self, job: Job, code: int, on_done) -> None:
        result = parse_output(job.lines)
        if code != 0 and not result.get("error"):
            tail = [t for s, t in job.lines if s != "sys" and t.strip()]
            result["error"] = tail[-1] if tail else "install.sh exited with code %d" % code
        with job.cond:
            job.exit_code = code
            job.result = result
        # bookkeeping first, so that a UI refreshing on "done" already sees the recorded state
        if on_done:
            try:
                on_done(job)
            except Exception as e:  # keep the job record even if bookkeeping fails
                job.add("err", "launcher: could not record the result: %s" % e)
        with job.cond:
            job.state = "done" if code == 0 else "failed"
            job.ended = time.time()
            job.cond.notify_all()
        for fn in list(self.listeners):
            try:
                fn(job)
            except Exception:
                pass

    def wait(self, job: Job, timeout: float | None = None) -> bool:
        deadline = None if timeout is None else time.time() + timeout
        with job.cond:
            while job.state == "running":
                left = None if deadline is None else deadline - time.time()
                if left is not None and left <= 0:
                    return False
                job.cond.wait(left if left is not None else 1.0)
        return True


def _quote(a: str) -> str:
    if re.fullmatch(r"[A-Za-z0-9_./:=,+-]+", a or ""):
        return a
    return "'" + str(a).replace("'", "'\\''") + "'"


def path_ok_for_dll(p: str) -> bool:
    q = Path(p)
    return q.is_file() and q.suffix.lower() in (".dll", ".zip")
