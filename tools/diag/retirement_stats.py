"""Summarize bounded AMD-S4 retirement captures without inferring display FPS.

Usage: python -B tools/diag/retirement_stats.py PATH_TO_AMD_PRESR_LOG [--capture ID]
Only the Python standard library is required. Unknown fields are accepted, but
schema 1 fields are required so a damaged row cannot silently change a blocker.
"""

import argparse
from collections import Counter
from dataclasses import dataclass, field
import math
from pathlib import Path
import re
import statistics
import sys


UINT64_MAX = (1 << 64) - 1
MARKER = re.compile(r"\[AMD-S4(?P<kind>-BEGIN|-END)?\]\s*(?P<body>.*)$")
SOURCES = {"Record", "Status", "Ready", "Submitted", "EveryFrameWait", "Shutdown"}
OUTCOMES = {"recorded", "pending_skip", "unsubmitted_skip", "fence_skip", "other_skip", "poll"}
INTEGER_FIELDS = (
    "capture attempt pending submitted passes accepted native done0 job0 done1 job1 done2 job2 "
    "gpu_before gpu_after target retired extra_gpu_before extra_gpu_after extra_target "
    "extra_waited width height every_frame recorded_at submitted_at"
).split()
FLOAT_FIELDS = "t_ms wait_ms extra_wait_ms".split()
BOOL_FIELDS = "submitted native retired extra_waited every_frame".split()


@dataclass
class Capture:
    capture_id: int
    begin_line: int
    runtime: str
    window_ms: float
    rows: list = field(default_factory=list)
    raw_rows: int = 0
    end: dict | None = None
    warnings: list = field(default_factory=list)


def fields(text):
    result = {}
    for token in text.split():
        key, separator, value = token.partition("=")
        if not separator or not key or not value:
            raise ValueError(f"invalid field {token!r}")
        if key in result:
            raise ValueError(f"duplicate field {key}")
        result[key] = value
    return result


def integer(data, key):
    value = data[key]
    number = int(value, 16 if value.lower().startswith("0x") else 10)
    if not 0 <= number <= UINT64_MAX:
        raise ValueError(f"{key} is outside uint64")
    return number


def number(data, key):
    result = float(data[key])
    if not math.isfinite(result) or result < 0:
        raise ValueError(f"{key} must be finite and nonnegative")
    return result


def parse_row(data):
    row = {key: integer(data, key) for key in INTEGER_FIELDS}
    row.update({key: number(data, key) for key in FLOAT_FIELDS})
    row["source"], row["outcome"] = data["source"], data["outcome"]
    if row["source"] not in SOURCES or row["outcome"] not in OUTCOMES:
        raise ValueError("unknown source or outcome")
    if (any(row[key] not in (0, 1) for key in BOOL_FIELDS)
            or row["passes"] > 3 or row["accepted"] > 3):
        raise ValueError("invalid boolean or pass count")
    if (row["source"] == "Record") == (row["outcome"] == "poll"):
        raise ValueError("Record must have an outcome; other sources must use poll")
    return row


def parse_captures(lines):
    captures, warnings = [], []
    active = None
    for line_no, line in enumerate(lines, 1):
        match = MARKER.search(line)
        if not match:
            continue
        kind = match["kind"] or "row"
        # A row that failed parsing still counts as a physical row for END.rows.
        if kind == "row" and active is not None:
            active.raw_rows += 1
        try:
            data = fields(match["body"])
            capture_id = integer(data, "capture")
            if kind == "-BEGIN":
                if integer(data, "schema") != 1:
                    raise ValueError("unsupported schema")
                window = number(data, "window_ms")
                runtime = data["runtime"]
                if active is not None:
                    active.warnings.append(f"truncated before next BEGIN at line {line_no}")
                active = Capture(capture_id, line_no, runtime, window)
                captures.append(active)
            elif active is None or active.capture_id != capture_id:
                warnings.append(f"line {line_no}: orphan or mismatched {kind}, capture={capture_id}")
            elif kind == "-END":
                reason = data["reason"]
                if reason not in {"window", "capacity", "shutdown"}:
                    raise ValueError("invalid END reason")
                active.end = {
                    "reason": reason,
                    "rows": integer(data, "rows"),
                    "elapsed_ms": number(data, "elapsed_ms"),
                }
                if active.end["rows"] != active.raw_rows:
                    active.warnings.append(
                        f"END.rows={active.end['rows']} differs from {active.raw_rows} physical rows"
                    )
                if reason == "capacity":
                    active.warnings.append("capacity reached: capture window was truncated")
                active = None
            else:
                row = parse_row(data)
                row["line"] = line_no
                active.rows.append(row)
        except (ValueError, KeyError) as error:
            message = f"line {line_no}: malformed {kind}: {error}"
            (active.warnings if active is not None else warnings).append(message)
    if active is not None:
        active.warnings.append("truncated at EOF: no valid END")
    return captures, warnings


def select_capture(captures, capture_id=None):
    candidates = [c for c in captures if capture_id is None or c.capture_id == capture_id]
    complete = [c for c in candidates if c.end is not None]
    if not complete:
        suffix = "" if capture_id is None else f" {capture_id}"
        raise ValueError(f"no complete capture{suffix}; BEGIN and valid END are required")
    return complete[-1]


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower, upper = math.floor(position), math.ceil(position)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def wait_stats(values):
    return {
        "n": len(values),
        "positive": sum(v > 0 for v in values),
        "mean": statistics.mean(values) if values else None,
        "p50": percentile(values, .50),
        "p95": percentile(values, .95),
        "max": max(values) if values else None,
        "over": {limit: sum(v > limit for v in values) for limit in (1, 2, 5, 16)},
    }


def blockers(row):
    """Independent states actually used at the last retirement check."""
    submitted = "submitted" if row["submitted"] else "unsubmitted"
    native = ("no_native_work" if row["passes"] == 0 else
              "native_done" if row["native"] else "native_pending")
    if row["gpu_after"] == UINT64_MAX:
        gpu = "device_removed"
    elif not row["submitted"]:
        # Before Submit, target can still refer to the PREVIOUS list.
        gpu = "fence_unassigned"
    else:
        gpu = "fence_done" if row["gpu_after"] >= row["target"] else "fence_pending"
    return submitted, native, gpu


def summarize(capture):
    records, seen, duplicate_attempts = [], set(), Counter()
    for row in capture.rows:
        if row["source"] != "Record":
            continue
        if row["attempt"] in seen:
            duplicate_attempts[row["attempt"]] += 1
            continue
        seen.add(row["attempt"])
        records.append(row)
    outcomes = Counter(row["outcome"] for row in records)
    matrix = Counter(blockers(row) for row in records if row["outcome"] == "pending_skip")
    waits = {}
    for name, group in (
        ("all Record", records),
        ("recorded", [r for r in records if r["outcome"] == "recorded"]),
        ("not recorded", [r for r in records if r["outcome"] != "recorded"]),
    ):
        waits[name] = wait_stats([r["wait_ms"] + r["extra_wait_ms"] for r in group])
    waits["retirement only"] = wait_stats([r["wait_ms"] for r in records])
    waits["extra fence only"] = wait_stats([r["extra_wait_ms"] for r in records])
    return {
        "records": records,
        "outcomes": outcomes,
        "blockers": matrix,
        "waits": waits,
        "sources": Counter(r["source"] for r in capture.rows if r["source"] != "Record"),
        "duplicate_attempts": duplicate_attempts,
        "settings": Counter((r["width"], r["height"], r["passes"], r["every_frame"]) for r in records),
        "retired_during_record": sum(r["retired"] for r in records),
        "accepted_records": sum(r["accepted"] > 0 for r in records),
        "accepted_passes": Counter(r["accepted"] for r in records),
    }


def render_report(capture, report, warnings=()):
    end = capture.end
    records = report["records"]
    count = len(records)
    lines = [
        f"capture={capture.capture_id} runtime={capture.runtime} reason={end['reason']} "
        f"elapsed_ms={end['elapsed_ms']:.1f} window_ms={capture.window_ms:g}",
        f"rows: physical={capture.raw_rows} valid={len(capture.rows)} Record={count} "
        f"duplicate_Record={sum(report['duplicate_attempts'].values())}",
        "Record outcomes (unique attempts): " + ", ".join(
            f"{key}={report['outcomes'][key]} ({100 * report['outcomes'][key] / count:.1f}%)"
            if count else f"{key}=0"
            for key in ("recorded", "pending_skip", "unsubmitted_skip", "fence_skip", "other_skip")
        ),
        f"Record attempts with native accepted>0={report['accepted_records']} "
        "(not completions or displayed frames); accepted passes per attempt: " +
        (", ".join(f"{passes}={n}" for passes, n in sorted(report["accepted_passes"].items())) or "none"),
        f"retired during Record={report['retired_during_record']}; non-Record observations: " +
        (", ".join(f"{k}={v}" for k, v in sorted(report["sources"].items())) or "none"),
        "Record snapshot dimensions/passes/every_frame: " +
        (", ".join(f"{w}x{h}/p{p}/ef{e}={n}" for (w, h, p, e), n in sorted(report["settings"].items())) or "none"),
        "pending_skip matrix (independent states; GPU after retirement wait):",
    ]
    denominator = report["outcomes"]["pending_skip"]
    for (submitted, native, gpu), n in sorted(report["blockers"].items()):
        lines.append(f"  {submitted:11} {native:14} {gpu:16} {n:6} ({100*n/denominator:.1f}%)")
    if not denominator:
        lines.append("  none")
    lines.append("Record wait ms (includes zero waits; total = retirement + extra fence):")
    for name, values in report["waits"].items():
        if not values["n"]:
            lines.append(f"  {name}: n=0")
            continue
        over = "/".join(str(values["over"][limit]) for limit in (1, 2, 5, 16))
        lines.append(
            f"  {name:16} n={values['n']} waited={values['positive']} "
            f"mean/p50/p95/max={values['mean']:.3f}/{values['p50']:.3f}/"
            f"{values['p95']:.3f}/{values['max']:.3f} >1/2/5/16ms={over}"
        )
    notes = list(warnings) + list(capture.warnings)
    if report["duplicate_attempts"]:
        notes.append("duplicate Record attempts excluded after first occurrence: " + ", ".join(
            f"{attempt}(+{n})" for attempt, n in sorted(report["duplicate_attempts"].items())
        ))
    for note in notes:
        lines.append("WARNING: " + note)
    lines.append("These are Record/retirement observations, not display FPS or GPU kernel timings.")
    lines.append("fence_pending with native_done does not establish Execute splitting as its cause.")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="amd_presr.log containing buffered AMD-S4 captures")
    parser.add_argument("--capture", type=int, help="select a completed capture ID instead of the last complete one")
    args = parser.parse_args(argv)
    try:
        with args.log.open(encoding="utf-8-sig", errors="replace") as stream:
            captures, warnings = parse_captures(stream)
        capture = select_capture(captures, args.capture)
        for other in captures:
            if other is not capture and other.end is None:
                warnings.append(f"ignored incomplete capture={other.capture_id} at line {other.begin_line}")
        print(render_report(capture, summarize(capture), warnings))
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
