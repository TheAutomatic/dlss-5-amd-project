#!/usr/bin/env python3
"""Compare real offline RR artifacts; execution PASS alone is never quality PASS.

Required evidence is emitted by offline_chain.exe: run.json, metrics.csv,
temporal.csv and packed dispatch_controls.bin. Demodulated lobes have no RGB
reference and are excluded from quality. Raw-input metrics are diagnostics only.
Noise (truth residual, not pure stochastic noise), detail, energy and temporal
results stay separate. Bounds are evaluated per frame, not just on averages.

A --policy JSON array may authorize EXACT old/new values for named settings,
RR context fields or derived controls, with a reason. Such a run is reported as
a policy comparison, NEVER identical math. Input/provider/driver drift cannot be
waived. Candidate-only features do not inherit permission to change defaults.

Exit: 0 bounded regression comparison (not absolute/native-game quality proof),
1 unusable/incomplete evidence (including unassessed temporal quality),
2 unauthorized identity/config drift, 4 quality regression. No-ground-truth
captures cannot receive quality acceptance; this tool has no image-only PASS.

Regression command (standard library only):
  python -m unittest discover -s tests/fsr-rr -p test_compare_runs.py
"""

import argparse
import csv
import hashlib
import json
import math
import struct
import sys
from pathlib import Path

# These cannot be waived by a settings policy. Paths/labels/closure names are
# provenance, not identity: a separately exported frozen closure must be allowed.
IDENTITY_KEYS = (
    "adapter", "adapter_luid", "adapter_vendor_id", "adapter_driver_version",
    "denoiser_sha256", "denoiser_size",
    "upscaler_sha256", "upscaler_size",
    "diffuse_signal", "specular_signal", "seed", "frames_per_scene", "scenes",
    "readback_every", "input_sha256", "denoiser_defaults", "metric_truth",
    "metric_channels_contract", "noise_model", "mode", "fixture_controls",
)
CONFIG_KEYS = ("rr_context", "effective_settings", "derived_controls",
               "converter_generation", "feature_case", "comparison_scope")
TRUTH = {"raw": "jittered-render-rgb", "presr": "jittered-render-rgb",
         "sr": "unjittered-target-rgb"}
LOBE_STAGES = {"lobe_diffuse", "lobe_specular"}
METRICS = ("rmse_luma", "bias_mean", "energy_ratio", "detail_ratio",
           "psnr_luma_db", "noise_std")
METRIC_CHANNELS_CONTRACT = "rgb-radiance-v1;nonfinite=rgba"
RESIDUAL_TEMPORAL = {"presr_residual_temporal_mad_per_readback",
                     "sr_residual_temporal_mad_per_readback"}
GHOST_TEMPORAL = {"presr_ghost_early_rmse", "presr_ghost_settled_rmse"}
GHOST_FRAME_FIELDS = {"presr_ghost_early_frame", "presr_ghost_settled_frame", "presr_ghost_reveal_frame"}
CONTROL_BYTES = 208
RAW_CONTROL_BYTES = 200  # trailing srNearFar is derived from converter depth


class EvidenceError(ValueError):
    pass


def required(obj, key):
    if key not in obj or obj[key] is None or obj[key] == "" or obj[key] == {} or obj[key] == []:
        raise EvidenceError(f"missing evidence: {key}")
    return obj[key]


def finite(value, name):
    if isinstance(value, bool):
        raise EvidenceError(f"invalid number: {name}")
    try:
        number = float(value)
    except (ValueError, TypeError, OverflowError) as exc:
        raise EvidenceError(f"invalid number: {name}") from exc
    if not math.isfinite(number):
        raise EvidenceError(f"non-finite metric: {name}")
    return number


def integer(value, name, minimum=0):
    number = finite(value, name)
    if number < minimum or number != int(number):
        raise EvidenceError(f"invalid integer: {name}")
    return int(number)


def sha256(value, name):
    if not isinstance(value, str) or len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
        raise EvidenceError(f"invalid SHA256: {name}")
    return value


def read_csv(path):
    if not path.is_file():
        raise EvidenceError(f"missing evidence: {path.name}")
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def load_run(path):
    path = Path(path)
    data = json.loads((path / "run.json").read_text(encoding="utf-8"))
    controls = (path / "dispatch_controls.bin").read_bytes()
    if not controls or len(controls) % CONTROL_BYTES:
        raise EvidenceError("dispatch controls missing/empty or unexpected packed record size")
    raw_controls = b"".join(controls[i:i + RAW_CONTROL_BYTES]
                            for i in range(0, len(controls), CONTROL_BYTES))
    return {"json": data, "metrics": read_csv(path / "metrics.csv"),
            "temporal": read_csv(path / "temporal.csv"), "controls": controls,
            "controls_sha256": hashlib.sha256(controls).hexdigest(),
            "input_controls_sha256": hashlib.sha256(raw_controls).hexdigest()}


def leaves(value, path):
    if isinstance(value, dict) and value:
        out = {}
        for key, item in value.items():
            out.update(leaves(item, f"{path}.{key}"))
        return out
    return {path: value}


def same_value(left, right):
    # JSON booleans are not numeric settings (Python otherwise equates False/0).
    if isinstance(left, bool) != isinstance(right, bool):
        return False
    if isinstance(left, dict) and isinstance(right, dict):
        return set(left) == set(right) and all(same_value(left[k], right[k]) for k in left)
    if isinstance(left, list) and isinstance(right, list):
        return len(left) == len(right) and all(same_value(a, b) for a, b in zip(left, right))
    return left == right


def validate_policy(policy):
    if not isinstance(policy, list):
        raise EvidenceError("policy must be a JSON array")
    entries = {}
    for entry in policy:
        if not isinstance(entry, dict) or set(entry) != {"path", "baseline", "candidate", "reason"}:
            raise EvidenceError("policy entries require path/baseline/candidate/reason")
        path = entry["path"]
        if not isinstance(path, str) or path.split(".")[0] not in CONFIG_KEYS:
            raise EvidenceError("policy may only authorize settings/context/derived-control changes")
        if path in entries or not isinstance(entry["reason"], str) or not entry["reason"].strip():
            raise EvidenceError("duplicate policy path or empty reason")
        if same_value(entry["baseline"], entry["candidate"]):
            raise EvidenceError("policy must authorize a real difference")
        entries[path] = entry
    return entries


def guide_bindings(data):
    """Project actual input bindings, not derived plans or output readback hashes."""
    records = required(data, "feature_controls")
    if not isinstance(records, list):
        raise EvidenceError("feature_controls must be an array")
    passes = ("clean",) if data.get("capture_input_class") else ("clean", "noisy")
    frames = integer(data["frames_per_scene"], "frames_per_scene", 2)
    expected = [(scene, pss, frame) for scene in data["scenes"] for pss in passes
                for frame in range(frames)]
    if len(records) != len(expected):
        raise EvidenceError("guide binding sequence does not cover all scene/pass/frames")
    bindings = []
    for record, key in zip(records, expected):
        if not isinstance(record, dict):
            raise EvidenceError("feature_controls records must be objects")
        frame = record.get("frame")
        if type(frame) is not int or (record.get("scene"), record.get("pass"), frame) != key:
            raise EvidenceError("guide binding scene/pass/frame sequence is incomplete")
        for name in ("specular_guide", "diffuse_guide"):
            if type(record.get(name)) is not bool:
                raise EvidenceError(f"missing/invalid per-frame guide binding: {name}")
        bindings.append({name: record[name] for name in
                         ("scene", "pass", "frame", "specular_guide", "diffuse_guide")})
    return bindings


def validate_identity(run):
    data = run["json"]
    if data.get("execution_result") != "PASS":
        raise EvidenceError("run did not reach execution PASS")
    for key in IDENTITY_KEYS + CONFIG_KEYS:
        required(data, key)
    if data["metric_channels_contract"] != METRIC_CHANNELS_CONTRACT:
        raise EvidenceError("unsupported metric channel contract; regenerate RGB radiance metrics")
    for key in ("denoiser_sha256", "upscaler_sha256", "input_sha256"):
        sha256(data[key], key)
    if "input_controls_sha256" in data:
        sha256(data["input_controls_sha256"], "input_controls_sha256")
        if data["input_controls_sha256"] != run["input_controls_sha256"]:
            raise EvidenceError("input-controls hash disagrees with dispatch_controls.bin")
    for key in ("rr_context", "effective_settings", "derived_controls", "denoiser_defaults", "metric_truth"):
        if not isinstance(data[key], dict):
            raise EvidenceError(f"{key} must be an object")
    derived_hash = hashlib.sha256(b"".join(
        run["controls"][i + RAW_CONTROL_BYTES:i + CONTROL_BYTES]
        for i in range(0, len(run["controls"]), CONTROL_BYTES))).hexdigest()
    recorded_hash = required(data["derived_controls"], "sr_near_far_sha256")
    if sha256(recorded_hash, "derived_controls.sr_near_far_sha256") != derived_hash:
        raise EvidenceError("derived SR near/far hash disagrees with dispatch_controls.bin")
    for name, default in data["denoiser_defaults"].items():
        if not isinstance(default, dict) or default.get("queried") is not True:
            raise EvidenceError(f"provider default was not actually queried: {name}")
        finite(required(default, "value"), f"denoiser_defaults.{name}")
    for key in ("validation_errors", "sdk_errors"):
        if integer(required(data, key), key) != 0:
            raise EvidenceError(f"run has {key}")
    if data["mode"] != "serial":
        raise EvidenceError("quality metrics require serial readback mode")
    frames = integer(data["frames_per_scene"], "frames_per_scene", 2)
    integer(data["readback_every"], "readback_every", 1)
    scenes = data["scenes"]
    if not isinstance(scenes, list) or any(not isinstance(s, str) or not s for s in scenes) or len(set(scenes)) != len(scenes):
        raise EvidenceError("invalid/duplicate scene names")
    fixture = data["fixture_controls"]
    if not isinstance(fixture, dict) or fixture.get("guides") not in (
            "existing", "both", "none", "specular", "dropout", "arrival"):
        raise EvidenceError("missing/invalid fixture_controls.guides")
    for name in ("capture", "failure_recovery"):
        if type(fixture.get(name)) is not bool:
            raise EvidenceError(f"missing/invalid fixture_controls.{name}")
    guide_bindings(data)
    passes = 1 if data.get("capture_input_class") else 2
    if len(run["controls"]) != len(scenes) * passes * frames * CONTROL_BYTES:
        raise EvidenceError("dispatch control count does not cover all scene/pass/frames")
    # Frame IDs also detect truncated/repeated control streams with the same length.
    ids = [struct.unpack_from("<I", run["controls"], i)[0]
           for i in range(0, len(run["controls"]), CONTROL_BYTES)]
    if ids != list(range(frames)) * (len(scenes) * passes):
        raise EvidenceError("dispatch control frame sequence is incomplete")


def compare_identity(base, cand, policy):
    drift, approved = [], []
    bjson, cjson = base["json"], cand["json"]
    for key in IDENTITY_KEYS:
        if not same_value(bjson[key], cjson[key]):
            drift.append(f"{key}: baseline={bjson[key]!r} candidate={cjson[key]!r}")
    # Capture resource hashes/camera metadata must match, not just the capture path.
    for key in ("capture_input_class", "capture_provenance", "capture_frames",
                "denoiser_file_version", "upscaler_file_version"):
        if not same_value(bjson.get(key), cjson.get(key)):
            drift.append(f"{key} differs")
    if base["input_controls_sha256"] != cand["input_controls_sha256"]:
        drift.append("raw applied dispatch controls differ")
    if not same_value(guide_bindings(bjson), guide_bindings(cjson)):
        drift.append("actual per-frame guide bindings differ")
    used = set()
    for root in CONFIG_KEYS:
        bl, cl = leaves(bjson[root], root), leaves(cjson[root], root)
        for path in sorted(set(bl) | set(cl)):
            b, c = bl.get(path), cl.get(path)
            if path in bl and path in cl and same_value(b, c):
                continue
            entry = policy.get(path)
            if entry and same_value(entry["baseline"], b) and same_value(entry["candidate"], c):
                approved.append(entry)
                used.add(path)
            else:
                drift.append(f"{path}: baseline={b!r} candidate={c!r}")
    unused = set(policy) - used
    if unused:
        drift.append(f"policy entries not matched exactly: {sorted(unused)}")
    if base["controls_sha256"] != cand["controls_sha256"]:
        if not any(e["path"] == "derived_controls.sr_near_far_sha256" for e in approved):
            drift.append("derived applied SR controls differ without exact derived-controls policy")
    return drift, approved


def metric_rows(run):
    data, rows = run["json"], run["metrics"]
    if not rows:
        raise EvidenceError("no ground-truth metric rows; finiteness/image-only capture is not quality evidence")
    frames = integer(data["frames_per_scene"], "frames_per_scene", 2)
    cadence = integer(data["readback_every"], "readback_every", 1)
    captured = bool(data.get("capture_input_class"))
    expected_frames = set(range(frames)) if captured else set(range(0, frames, cadence)) | {frames - 1}
    passes = {"clean"} if captured else {"clean", "noisy"}
    stages = {"sr"} if captured else set(TRUTH)
    for stage in stages:
        if data["metric_truth"].get(stage) != TRUTH[stage]:
            raise EvidenceError(f"missing/invalid stage-specific RGB truth contract: {stage}")
    expected = {(s, p, f, stage) for s in data["scenes"] for p in passes
                for f in expected_frames for stage in stages}
    output, seen, excluded = {}, set(), 0
    for row in rows:
        scene, pss, stage = (required(row, k) for k in ("scene", "pass", "stage"))
        frame = integer(required(row, "frame"), "metric frame")
        key = (scene, pss, frame, stage)
        if key in seen:
            raise EvidenceError(f"duplicate metric row: {key}")
        seen.add(key)
        if scene not in data["scenes"] or pss not in passes or frame not in expected_frames:
            raise EvidenceError(f"unexpected scene/pass/frame: {key}")
        if integer(required(row, "nonfinite"), "nonfinite"):
            raise EvidenceError(f"non-finite GPU samples: {key}")
        values = {name: finite(required(row, name), f"{key}/{name}") for name in METRICS}
        if any(values[name] < 0 for name in ("rmse_luma", "detail_ratio", "noise_std")):
            raise EvidenceError(f"negative error/detail metric: {key}")
        if stage in LOBE_STAGES:
            excluded += 1  # demodulated signal != final RGB radiance, on both closures
            continue
        if stage not in stages:
            raise EvidenceError(f"unsupported quality stage: {stage}")
        output[key] = values
    if set(output) != expected:
        raise EvidenceError(f"metric frame/stage coverage mismatch: missing={len(expected - set(output))}, extra={len(set(output) - expected)}")
    return output, excluded


def temporal_rows(run):
    rows, result = run["temporal"], {}
    passes = {"clean"} if run["json"].get("capture_input_class") else {"clean", "noisy"}
    for row in rows:
        key = tuple(required(row, name) for name in ("scene", "pass", "metric"))
        if key in result:
            raise EvidenceError(f"duplicate temporal metric: {key}")
        if key[0] not in run["json"]["scenes"] or key[1] not in passes:
            raise EvidenceError(f"unexpected temporal scene/pass: {key}")
        result[key] = finite(required(row, "value"), f"temporal/{key}")
        if result[key] < 0:
            raise EvidenceError(f"negative temporal error: {key}")
    return result


def validate_ghost_frames(run, temporal):
    data = run["json"]
    if "occlusion_reveal" not in data["scenes"] or data.get("temporal_metrics_contract") != "truth-residual-v1":
        return True
    count = integer(data["frames_per_scene"], "frames_per_scene", 2)
    cadence = integer(data["readback_every"], "readback_every", 1)
    read_frames = sorted(set(range(0, count, cadence)) | {count - 1})
    for pss in ("clean", "noisy"):
        keys = {field: ("occlusion_reveal", pss, field) for field in GHOST_FRAME_FIELDS}
        if not all(key in temporal for key in keys.values()):
            return False
        values = {field: integer(temporal[key], field) for field, key in keys.items()}
        reveal = values["presr_ghost_reveal_frame"]
        eligible = [frame for frame in read_frames if frame >= reveal]
        if not eligible or eligible[0] == count - 1:
            raise EvidenceError("occlusion temporal evidence has no post-reveal settling interval")
        if (values["presr_ghost_early_frame"] != eligible[0]
                or values["presr_ghost_settled_frame"] != count - 1):
            raise EvidenceError("ghost measurements are not event-aligned first/settled readbacks")
    return True


def limits(**overrides):
    result = {"rmse_tolerance": 1.05, "psnr_drop_db": 0.5, "detail_band": 0.10,
              "energy_band": 0.02, "error_floor": 1e-6,
              "temporal_tolerance": 1.05, "temporal_floor": 1e-6}
    result.update(overrides)
    for name, value in result.items():
        finite(value, name)
        if value < (1.0 if name.endswith("tolerance") else 0.0):
            raise EvidenceError(f"invalid tolerance: {name}")
    return result


def analyze(base, cand, policy=None, bounds=None):
    policy = validate_policy([] if policy is None else policy)
    bounds = limits(**(bounds or {}))
    for run in (base, cand):
        validate_identity(run)
    drift, approved = compare_identity(base, cand, policy)
    bm, be = metric_rows(base)
    cm, ce = metric_rows(cand)
    if set(bm) != set(cm):
        drift.append("metric scene/pass/frame/stage keys differ")
    inventories = [
        {(r["scene"], r["pass"], integer(r["frame"], "metric frame"), r["stage"])
         for r in run["metrics"]} for run in (base, cand)
    ]
    if inventories[0] != inventories[1]:
        drift.append("metric row inventories differ (including excluded lobe diagnostics)")
    bt, ct = temporal_rows(base), temporal_rows(cand)
    if set(bt) != set(ct):
        drift.append("temporal metric keys differ")
    for key in set(bt) & set(ct):
        if key[2] in GHOST_FRAME_FIELDS and bt[key] != ct[key]:
            drift.append(f"temporal event/readback frame identity differs: {key}")
    report = {"comparison_kind": "policy" if approved else "identical-input-settings",
              "drift": drift, "approved_changes": approved, "bounds": bounds,
              "quality": [], "temporal": [], "raw_diagnostics": [],
              "excluded_lobe_rows": {"baseline": be, "candidate": ce},
              "regressions": [], "limitations": [
                  "Relative regression bounds do not prove the baseline itself is good or native-game parity.",
                  "noise_std is pooled RGB truth-residual spread, not isolated stochastic noise; alpha is checked only for finiteness.",
                  "Demodulated lobe-vs-final-RGB metrics are invalid and excluded.",
                  "Residual temporal MAD is not motion-compensated flicker; whole-image reveal error is not an isolated ghost ROI.",
                  "Raw-input truth errors are diagnostics, not reconstruction quality gates."]}
    report["evidence"] = {
        label: {"identity": {key: run["json"][key] for key in IDENTITY_KEYS},
                "configuration": {key: run["json"][key] for key in CONFIG_KEYS},
                "dispatch_controls_sha256": run["controls_sha256"],
                "input_controls_sha256": run["input_controls_sha256"],
                "guide_bindings": guide_bindings(run["json"]),
                "metric_rows": len(run["metrics"]), "temporal_rows": len(run["temporal"])}
        for label, run in (("baseline", base), ("candidate", cand))
    }
    failures = {"noise": [], "detail": [], "energy": [], "temporal": []}
    for key in sorted(set(bm) & set(cm)):
        b, c = bm[key], cm[key]
        scene, pss, frame, stage = key
        item = {"scene": scene, "pass": pss, "frame": frame, "stage": stage,
                "baseline": b, "candidate": c}
        if stage == "raw":
            report["raw_diagnostics"].append(item)
            continue
        local = {"noise": [], "detail": [], "energy": []}
        for name in ("rmse_luma", "noise_std"):
            if c[name] > b[name] * bounds["rmse_tolerance"] + bounds["error_floor"]:
                local["noise"].append(name)
        # PSNR's 999 sentinel is not a physical dB value; zero error is governed by
        # the explicit absolute error floor, rather than a sentinel-sized drop.
        if b["rmse_luma"] > bounds["error_floor"] and c["rmse_luma"] > bounds["error_floor"]:
            if c["psnr_luma_db"] < b["psnr_luma_db"] - bounds["psnr_drop_db"]:
                local["noise"].append("psnr_luma_db")
        # Improvement toward the analytic truth (ratio=1) is not a regression.
        if abs(c["detail_ratio"] - 1.0) > abs(b["detail_ratio"] - 1.0) + bounds["detail_band"]:
            local["detail"].append("detail_ratio")
        if abs(c["energy_ratio"] - 1.0) > abs(b["energy_ratio"] - 1.0) + bounds["energy_band"]:
            local["energy"].append("energy_ratio")
        if abs(c["bias_mean"]) > abs(b["bias_mean"]) + bounds["energy_band"]:
            local["energy"].append("bias_mean")
        item["results"] = {name: "REGRESSION" if fields else "WITHIN-BOUNDS" for name, fields in local.items()}
        for name, fields in local.items():
            if fields:
                failures[name].append({"key": list(key), "metrics": fields})
        report["quality"].append(item)
    valid_temporal = all(run["json"].get("temporal_metrics_contract") == "truth-residual-v1"
                         for run in (base, cand))
    expected_temporal = {(s, p, metric) for s in base["json"]["scenes"]
                         for p in ({"clean"} if base["json"].get("capture_input_class") else {"clean", "noisy"})
                         for metric in RESIDUAL_TEMPORAL}
    for scene in base["json"]["scenes"]:
        if scene == "occlusion_reveal":
            expected_temporal.update((scene, p, metric) for p in ("clean", "noisy") for metric in GHOST_TEMPORAL)
    if not expected_temporal.issubset(bt) or not expected_temporal.issubset(ct):
        valid_temporal = False
    ghost_frames_valid = [validate_ghost_frames(run, values)
                          for run, values in ((base, bt), (cand, ct))]
    valid_temporal = valid_temporal and all(ghost_frames_valid)
    for key in sorted(set(bt) & set(ct)):
        acceptance = valid_temporal and key in expected_temporal
        failed = acceptance and ct[key] > bt[key] * bounds["temporal_tolerance"] + bounds["temporal_floor"]
        if failed:
            failures["temporal"].append({"key": list(key), "metric": key[2]})
        report["temporal"].append({"scene": key[0], "pass": key[1], "metric": key[2],
                                   "baseline": bt[key], "candidate": ct[key], "delta": ct[key] - bt[key],
                                   "result": "REGRESSION" if failed else "WITHIN-BOUNDS" if acceptance else "ADVISORY"})
    report["results"] = {name: "REGRESSION" if values else "WITHIN-BOUNDS" for name, values in failures.items()}
    if not valid_temporal:
        report["results"]["temporal"] = "NOT-ASSESSED"
        report["limitations"].append("Legacy inter-image MAD includes scene motion; legacy ghost rows are not event-aligned. Neither is temporal quality evidence.")
    captured = bool(base["json"].get("capture_input_class") or cand["json"].get("capture_input_class"))
    if captured:
        report["limitations"].append("Clean raster capture is not a noisy ray-tracing fixture; no noise-removal acceptance.")
        if report["results"]["noise"] != "REGRESSION":
            report["results"]["noise"] = "NOT-ASSESSED"
    report["regressions"] = failures
    if drift:
        report["comparison_kind"] = "unmatched-input-or-settings"
        report["limitations"].append("Metrics are descriptive only while identity/config drift is unresolved.")
        report["results"] = {name: "NOT-COMPARABLE" for name in report["results"]}
    if drift:
        verdict, code = "CONFIG-DRIFT", 2
    elif any(failures.values()):
        verdict, code = "QUALITY-REGRESSION", 4
    elif not valid_temporal or captured:
        verdict, code = "PARTIAL-QUALITY-NOT-ACCEPTED", 1
    else:
        verdict, code = "QUALITY-WITHIN-BOUNDS", 0
    if approved:
        verdict = "POLICY-" + verdict
    report.update(verdict=verdict, quality_accepted=(code == 0), exit_code=code)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("candidate")
    parser.add_argument("--out")
    parser.add_argument("--policy", help="JSON array of exact authorized config/context old/new values and reasons")
    for name, value in limits().items():
        parser.add_argument("--" + name.replace("_", "-"), type=float, default=value)
    args = parser.parse_args(argv)
    try:
        policy = json.loads(Path(args.policy).read_text(encoding="utf-8")) if args.policy else []
        report = analyze(load_run(args.baseline), load_run(args.candidate), policy,
                         {name: getattr(args, name) for name in limits()})
        report.update(baseline=args.baseline, candidate=args.candidate)
        out = Path(args.out) if args.out else Path(args.candidate) / "compare"
        out.mkdir(parents=True, exist_ok=True)
        target = out / "compare.json"
        target.write_text(json.dumps(report, indent=2, allow_nan=False), encoding="utf-8")
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"FAIL: unusable comparison evidence: {exc}", file=sys.stderr)
        return 1
    print(f"compare: {report['verdict']} (report: {target})")
    for name, result in report["results"].items():
        print(f"  {name}: {result}")
    for change in report["approved_changes"]:
        print(f"  [policy] {change['path']}: {change['baseline']!r} -> {change['candidate']!r}: {change['reason']}")
    for drift in report["drift"]:
        print(f"  [drift] {drift}")
    return report["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
