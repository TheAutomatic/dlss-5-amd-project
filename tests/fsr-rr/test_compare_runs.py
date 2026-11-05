"""Focused comparison semantics tests; no GPU, external packages or mock GPU proof.

Run from checkout root:
    python -m unittest discover -s tests/fsr-rr -p test_compare_runs.py
These fixtures exercise artifact analysis only, never claim actual RR execution.
"""

import copy
import csv
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("compare_runs", Path(__file__).with_name("compare_runs.py"))
compare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compare)


def evidence():
    controls = b"".join(struct.pack("<I", frame) + bytes(204)
                        for _ in range(2) for frame in range(3))
    raw = b"".join(controls[i:i + 200] for i in range(0, len(controls), 208))
    data = {
        "adapter": "actual adapter description", "adapter_luid": 42,
        "adapter_vendor_id": 4098, "adapter_driver_version": "31.0.1.2",
        "denoiser_sha256": "a" * 64, "denoiser_size": 1200, "denoiser_file_version": "1.2",
        "upscaler_sha256": "b" * 64, "upscaler_size": 1300, "upscaler_file_version": "4.1",
        "diffuse_signal": "direct", "specular_signal": "indirect", "seed": 17,
        "frames_per_scene": 3, "readback_every": 1, "scenes": ["detail"],
        "input_sha256": "c" * 64, "input_controls_sha256": hashlib.sha256(raw).hexdigest(),
        "denoiser_defaults": {"stabilityBias": {"queried": True, "value": 0.0}},
        "rr_context": {"version": 12, "signalFlags": 3, "maxRenderSize": [64, 64]},
        "effective_settings": {"Floor": False, "Strength": 1.0},
        "derived_controls": {"sr_depth_source": "hardware",
                             "sr_near_far_sha256": hashlib.sha256(bytes(8 * 6)).hexdigest()},
        "fixture_controls": {"guides": "both", "capture": False, "failure_recovery": False},
        "feature_controls": [{"scene": "detail", "pass": pss, "frame": frame,
                              "specular_guide": True, "diffuse_guide": True,
                              "signal_mask": 3}
                             for pss in ("clean", "noisy") for frame in range(3)],
        "converter_generation": "frozen", "feature_case": "default",
        "comparison_scope": "baseline-compatible", "noise_model": "positive-unbiased",
        "metric_truth": dict(compare.TRUTH), "mode": "serial",
        "metric_channels_contract": compare.METRIC_CHANNELS_CONTRACT,
        "execution_result": "PASS", "validation_errors": 0, "sdk_errors": 0,
        "temporal_metrics_contract": "truth-residual-v1",
    }
    metrics = []
    for pss in ("clean", "noisy"):
        for frame in range(3):
            for stage in ("raw", "presr", "sr", "lobe_diffuse", "lobe_specular"):
                metrics.append({"scene": "detail", "pass": pss, "frame": str(frame), "stage": stage,
                                "rmse_luma": "0.1", "bias_mean": "0.001", "energy_ratio": "1.01",
                                "detail_ratio": "0.8", "psnr_luma_db": "20", "nonfinite": "0",
                                "noise_std": "0.1"})
    temporal = [{"scene": "detail", "pass": pss, "metric": metric, "value": "0.01"}
                for pss in ("clean", "noisy") for metric in sorted(compare.RESIDUAL_TEMPORAL)]
    return {"json": data, "controls": controls,
            "controls_sha256": hashlib.sha256(controls).hexdigest(),
            "input_controls_sha256": hashlib.sha256(raw).hexdigest(),
            "metrics": metrics, "temporal": temporal}


def change_row(run, name, value, stage="presr", frame="0", pss="noisy"):
    row = next(row for row in run["metrics"]
               if row["stage"] == stage and row["frame"] == frame and row["pass"] == pss)
    row[name] = str(value)


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.base = evidence()
        self.cand = copy.deepcopy(self.base)

    def result(self, **kwargs):
        return compare.analyze(self.base, self.cand, **kwargs)

    def test_complete_identical_artifacts_have_separate_results(self):
        report = self.result()
        self.assertEqual(report["exit_code"], 0)
        self.assertTrue(report["quality_accepted"])
        self.assertEqual(set(report["results"]), {"noise", "detail", "energy", "temporal"})
        self.assertEqual(len(report["quality"]), 12)
        self.assertEqual(report["excluded_lobe_rows"]["candidate"], 12)

    def test_missing_identity_on_both_sides_is_not_identity(self):
        for field in ("adapter_driver_version", "input_sha256", "effective_settings", "rr_context"):
            with self.subTest(field=field):
                base, cand = evidence(), evidence()
                del base["json"][field]
                del cand["json"][field]
                with self.assertRaises(compare.EvidenceError):
                    compare.analyze(base, cand)

    def test_missing_legacy_and_mismatched_metric_channels_are_rejected(self):
        for side in ("baseline", "candidate", "both"):
            for contract in (None, "rgba-v1", "rgb-radiance-v2;nonfinite=rgba"):
                with self.subTest(side=side, contract=contract):
                    base, cand = evidence(), evidence()
                    runs = (base, cand) if side == "both" else (base,) if side == "baseline" else (cand,)
                    for run in runs:
                        if contract is None:
                            del run["json"]["metric_channels_contract"]
                        else:
                            run["json"]["metric_channels_contract"] = contract
                    with self.assertRaises(compare.EvidenceError):
                        compare.analyze(base, cand)

    def test_provider_defaults_must_have_been_queried(self):
        self.base["json"]["denoiser_defaults"]["stabilityBias"]["queried"] = False
        self.cand["json"]["denoiser_defaults"]["stabilityBias"]["queried"] = False
        with self.assertRaises(compare.EvidenceError):
            self.result()

    def test_actual_identity_changes_cannot_be_waived(self):
        for field, value in (("adapter_driver_version", "new"), ("input_sha256", "d" * 64),
                             ("denoiser_sha256", "e" * 64)):
            with self.subTest(field=field):
                cand = evidence()
                cand["json"][field] = value
                self.assertEqual(compare.analyze(self.base, cand)["exit_code"], 2)
        with self.assertRaises(compare.EvidenceError):
            self.result(policy=[{"path": "input_sha256", "baseline": "c" * 64,
                                 "candidate": "d" * 64, "reason": "not allowed"}])

    def test_same_authored_input_and_forced_direct_plan_do_not_hide_binding_drift(self):
        for guide in ("specular_guide", "diffuse_guide"):
            for change_fixture in (False, True):
                with self.subTest(guide=guide, change_fixture=change_fixture):
                    base, cand = evidence(), evidence()
                    for run in (base, cand):
                        run["json"]["effective_settings"].update(
                            FfxDenoiserDiffuseRoute=1, FfxDenoiserSpecularRoute=1)
                    cand["json"]["feature_controls"][1][guide] = False
                    if change_fixture:
                        cand["json"]["fixture_controls"]["guides"] = "dropout"
                    cand["json"]["effective_settings"]["Floor"] = True
                    policy = [{"path": "effective_settings.Floor", "baseline": False,
                               "candidate": True, "reason": "authorized setting only"}]
                    self.assertEqual(base["json"]["input_sha256"], cand["json"]["input_sha256"])
                    self.assertEqual(base["controls"], cand["controls"])
                    self.assertEqual(base["json"]["rr_context"], cand["json"]["rr_context"])
                    self.assertEqual([r["signal_mask"] for r in base["json"]["feature_controls"]],
                                     [r["signal_mask"] for r in cand["json"]["feature_controls"]])
                    report = compare.analyze(base, cand, policy=policy)
                    self.assertEqual(report["exit_code"], 2)
                    self.assertFalse(report["quality_accepted"])
                    self.assertIn("actual per-frame guide bindings differ", report["drift"])
                    self.assertTrue(all(v == "NOT-COMPARABLE" for v in report["results"].values()))

    def test_fixture_controls_are_required_nonwaivable_input_identity(self):
        self.cand["json"]["fixture_controls"]["guides"] = "none"
        self.assertEqual(self.result()["exit_code"], 2)
        for path in ("fixture_controls.guides", "feature_controls"):
            with self.subTest(path=path), self.assertRaises(compare.EvidenceError):
                self.result(policy=[{"path": path, "baseline": "both", "candidate": "none",
                                     "reason": "must not waive inputs"}])
        for field in ("fixture_controls", "feature_controls"):
            with self.subTest(field=field):
                base, cand = evidence(), evidence()
                del base["json"][field]
                del cand["json"][field]
                with self.assertRaises(compare.EvidenceError):
                    compare.analyze(base, cand)

    def test_binding_evidence_requires_complete_ordered_boolean_frame_records(self):
        mutations = (
            lambda rows: rows.clear(),
            lambda rows: rows.pop(),
            lambda rows: rows.append(dict(rows[0])),
            lambda rows: rows.reverse(),
            lambda rows: rows[1].update(rows[0]),
            lambda rows: rows[0].update(scene="other"),
            lambda rows: rows[0].update({"pass": "noisy"}),
            lambda rows: rows[0].update(frame=9),
            lambda rows: rows[0].update(frame=False),
            lambda rows: rows[0].pop("diffuse_guide"),
            lambda rows: rows[0].update(specular_guide=1),
            lambda rows: rows.__setitem__(0, None),
        )
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                base, cand = evidence(), evidence()
                for run in (base, cand):
                    mutate(run["json"]["feature_controls"])
                with self.assertRaises(compare.EvidenceError):
                    compare.analyze(base, cand)

    def test_derived_feature_plan_fields_are_not_input_bindings(self):
        self.cand["json"]["feature_controls"][0].update(
            signal_mask=1, plan_notes="different derived plan", presr_readback_sha256="d" * 64)
        self.assertEqual(self.result()["exit_code"], 0)

    def test_empty_missing_duplicate_and_wrong_frame_rows_are_invalid(self):
        mutations = [lambda r: r.clear(), lambda r: r.pop(),
                     lambda r: r.append(dict(r[0])), lambda r: r[0].update(frame="9")]
        # Pop a quality row rather than a semantically excluded lobe diagnostic.
        mutations[1] = lambda r: r.pop(0)
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                cand = evidence()
                mutate(cand["metrics"])
                with self.assertRaises(compare.EvidenceError):
                    compare.analyze(self.base, cand)

    def test_nan_inf_and_nonfinite_samples_never_average_away(self):
        for field, value in (("rmse_luma", "nan"), ("noise_std", "inf"),
                             ("detail_ratio", ""), ("nonfinite", "1")):
            with self.subTest(field=field):
                cand = evidence()
                change_row(cand, field, value)
                with self.assertRaises(compare.EvidenceError):
                    compare.analyze(self.base, cand)

    def test_local_frame_noise_regression_cannot_hide_in_average(self):
        change_row(self.cand, "noise_std", 0.13)
        # Better other frames cannot cancel a bad frame.
        change_row(self.cand, "noise_std", 0.0, frame="1")
        report = self.result()
        self.assertEqual(report["exit_code"], 4)
        self.assertEqual(report["results"]["noise"], "REGRESSION")
        self.assertEqual(report["results"]["energy"], "WITHIN-BOUNDS")

    def test_detail_improvement_is_not_failure_but_blur_is(self):
        change_row(self.cand, "detail_ratio", 1.0)
        self.assertEqual(self.result()["exit_code"], 0)
        change_row(self.cand, "detail_ratio", 0.3)
        self.assertEqual(self.result()["results"]["detail"], "REGRESSION")

    def test_zero_baseline_detail_does_not_disable_guard(self):
        change_row(self.base, "detail_ratio", 0.0)
        change_row(self.cand, "detail_ratio", 3.0)
        self.assertEqual(self.result()["results"]["detail"], "REGRESSION")

    def test_energy_and_signed_bias_not_normalized_or_averaged_away(self):
        change_row(self.cand, "bias_mean", 0.05)
        change_row(self.cand, "bias_mean", -0.05, frame="1")
        self.assertEqual(self.result()["results"]["energy"], "REGRESSION")

    def test_lobe_rgb_truth_metrics_and_raw_error_are_not_quality(self):
        for stage in ("lobe_diffuse", "lobe_specular", "raw"):
            change_row(self.cand, "rmse_luma", 100, stage=stage)
            change_row(self.cand, "detail_ratio", 100, stage=stage)
            change_row(self.cand, "energy_ratio", 100, stage=stage)
            change_row(self.cand, "psnr_luma_db", -20, stage=stage)
        self.assertEqual(self.result()["exit_code"], 0)

    def test_stage_specific_truth_contract_is_required(self):
        for run in (self.base, self.cand):
            run["json"]["metric_truth"]["presr"] = "unjittered-target-rgb"
        with self.assertRaises(compare.EvidenceError):
            self.result()

    def test_zero_error_psnr_sentinel_uses_explicit_error_floor(self):
        for run in (self.base, self.cand):
            change_row(run, "rmse_luma", 0)
            change_row(run, "noise_std", 0)
            change_row(run, "psnr_luma_db", 999)
        change_row(self.cand, "rmse_luma", 1e-8)
        change_row(self.cand, "psnr_luma_db", 160)
        self.assertEqual(self.result()["exit_code"], 0)
        change_row(self.cand, "rmse_luma", 0.001)
        self.assertEqual(self.result()["exit_code"], 4)

    def test_temporal_regression_is_independent_acceptance_gate(self):
        self.cand["temporal"][0]["value"] = "0.2"
        report = self.result()
        self.assertEqual(report["exit_code"], 4)
        self.assertEqual(report["results"]["temporal"], "REGRESSION")
        self.assertEqual(report["results"]["noise"], "WITHIN-BOUNDS")

    def test_temporal_empty_missing_duplicate_nan_never_pass(self):
        for mutate in (lambda r: r.clear(), lambda r: r.pop(),
                       lambda r: r.append(dict(r[0])), lambda r: r[0].update(value="nan")):
            with self.subTest(mutate=mutate):
                cand = evidence()
                mutate(cand["temporal"])
                try:
                    report = compare.analyze(self.base, cand)
                except compare.EvidenceError:
                    continue
                self.assertFalse(report["quality_accepted"])
                self.assertNotEqual(report["exit_code"], 0)

    def test_legacy_temporal_mad_cannot_claim_flicker_quality(self):
        for run in (self.base, self.cand):
            del run["json"]["temporal_metrics_contract"]
            run["temporal"] = [{"scene": "detail", "pass": "clean",
                                "metric": "presr_temporal_mad_per_readback", "value": "0.01"}]
        report = self.result()
        self.assertEqual(report["results"]["temporal"], "NOT-ASSESSED")
        self.assertFalse(report["quality_accepted"])

    def test_exact_policy_reports_setting_change_not_identical_math(self):
        self.cand["json"]["effective_settings"]["Floor"] = True
        self.assertEqual(self.result()["exit_code"], 2)
        policy = [{"path": "effective_settings.Floor", "baseline": False,
                   "candidate": True, "reason": "explicit frozen-to-candidate policy"}]
        report = self.result(policy=policy)
        self.assertEqual(report["exit_code"], 0)
        self.assertEqual(report["comparison_kind"], "policy")
        self.assertEqual(len(report["approved_changes"]), 1)
        # Authorization of one setting does not hide another default change.
        self.cand["json"]["effective_settings"]["Strength"] = 0.8
        self.assertEqual(self.result(policy=policy)["exit_code"], 2)

    def test_candidate_only_feature_context_is_not_blanket_permission(self):
        self.cand["json"]["comparison_scope"] = "candidate-only-feature"
        self.cand["json"]["rr_context"]["newFeature"] = True
        policy = [{"path": "comparison_scope", "baseline": "baseline-compatible",
                   "candidate": "candidate-only-feature", "reason": "new feature exercise"}]
        self.assertEqual(self.result(policy=policy)["exit_code"], 2)
        policy.append({"path": "rr_context.newFeature", "baseline": None,
                       "candidate": True, "reason": "added typed context"})
        self.assertEqual(self.result(policy=policy)["exit_code"], 0)

    def test_unused_or_wrong_policy_is_drift(self):
        policy = [{"path": "effective_settings.Floor", "baseline": False,
                   "candidate": True, "reason": "must occur"}]
        self.assertEqual(self.result(policy=policy)["exit_code"], 2)

    def test_derived_depth_change_does_not_waive_raw_controls(self):
        policy = [{"path": "derived_controls.sr_depth_source", "baseline": "hardware",
                   "candidate": "encoded", "reason": "intentional candidate SR depth encoding"}]
        self.cand["json"]["derived_controls"]["sr_depth_source"] = "encoded"
        controls = bytearray(self.cand["controls"])
        struct.pack_into("<f", controls, 200, 1.0)
        self.cand["controls"] = bytes(controls)
        self.cand["controls_sha256"] = hashlib.sha256(controls).hexdigest()
        derived_hash = hashlib.sha256(b"".join(
            controls[i + 200:i + 208] for i in range(0, len(controls), 208))).hexdigest()
        self.cand["json"]["derived_controls"]["sr_near_far_sha256"] = derived_hash
        policy.append({"path": "derived_controls.sr_near_far_sha256",
                       "baseline": self.base["json"]["derived_controls"]["sr_near_far_sha256"],
                       "candidate": derived_hash, "reason": "exact encoded SR depth bounds"})
        self.assertEqual(self.result()["exit_code"], 2)
        self.assertEqual(self.result(policy=policy)["exit_code"], 0)
        # Raw controls differ despite an authorized encoded-depth change.
        self.cand["input_controls_sha256"] = "e" * 64
        self.cand["json"]["input_controls_sha256"] = "e" * 64
        self.assertEqual(self.result(policy=policy)["exit_code"], 2)

    def test_capture_without_ground_truth_has_no_quality_verdict(self):
        self.cand["json"]["capture_input_class"] = "clean-raster-engine-chain-not-noisy-RT"
        self.cand["metrics"] = []
        with self.assertRaises(compare.EvidenceError):
            self.result()

    def test_missing_empty_or_truncated_controls_fail_loader(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "run.json").write_text(json.dumps(self.base["json"]), encoding="utf-8")
            for payload in (None, b"", b"truncated"):
                with self.subTest(payload=payload):
                    if payload is not None:
                        (root / "dispatch_controls.bin").write_bytes(payload)
                    with self.assertRaises((OSError, compare.EvidenceError)):
                        compare.load_run(root)

    def test_identically_truncated_runs_cannot_pass_coverage(self):
        for run in (self.base, self.cand):
            run["metrics"] = [r for r in run["metrics"] if r["frame"] != "2"]
        with self.assertRaises(compare.EvidenceError):
            self.result()

    def test_identically_empty_temporal_runs_are_not_full_acceptance(self):
        self.base["temporal"].clear()
        self.cand["temporal"].clear()
        report = self.result()
        self.assertFalse(report["quality_accepted"])
        self.assertEqual(report["results"]["temporal"], "NOT-ASSESSED")

    def test_ghost_errors_are_separate_from_potentially_misleading_ratio(self):
        for run in (self.base, self.cand):
            run["json"]["scenes"] = ["occlusion_reveal"]
            for row in run["metrics"] + run["temporal"] + run["json"]["feature_controls"]:
                row["scene"] = "occlusion_reveal"
            for pss in ("clean", "noisy"):
                for metric in compare.GHOST_TEMPORAL:
                    run["temporal"].append({"scene": "occlusion_reveal", "pass": pss,
                                            "metric": metric, "value": "0.01"})
                run["temporal"].append({"scene": "occlusion_reveal", "pass": pss,
                                        "metric": "presr_ghost_residual_ratio", "value": "1"})
                for metric, value in (("presr_ghost_reveal_frame", 1),
                                      ("presr_ghost_early_frame", 1), ("presr_ghost_settled_frame", 2)):
                    run["temporal"].append({"scene": "occlusion_reveal", "pass": pss,
                                            "metric": metric, "value": str(value)})
        for row in self.cand["temporal"]:
            if row["metric"] in compare.GHOST_TEMPORAL:
                row["value"] = "0.1"  # both errors grow; ratio would stay identical
        report = self.result()
        self.assertEqual(report["results"]["temporal"], "REGRESSION")

    def test_pre_reveal_or_wrong_settled_ghost_frames_are_invalid(self):
        for early, settled in ((0, 2), (1, 1)):
            with self.subTest(early=early, settled=settled):
                run = evidence()
                run["json"]["scenes"] = ["occlusion_reveal"]
                temporal = {("occlusion_reveal", pss, field): value
                            for pss in ("clean", "noisy")
                            for field, value in (("presr_ghost_reveal_frame", 1),
                                                 ("presr_ghost_early_frame", early),
                                                 ("presr_ghost_settled_frame", settled))}
                with self.assertRaises(compare.EvidenceError):
                    compare.validate_ghost_frames(run, temporal)

    def test_missing_ghost_measurements_cannot_claim_occlusion_quality(self):
        for run in (self.base, self.cand):
            run["json"]["scenes"] = ["occlusion_reveal"]
            for row in run["metrics"] + run["temporal"] + run["json"]["feature_controls"]:
                row["scene"] = "occlusion_reveal"
        self.assertFalse(self.result()["quality_accepted"])

    def test_boolean_numeric_setting_drift_is_not_silent(self):
        self.cand["json"]["effective_settings"]["Floor"] = 0
        self.assertEqual(self.result()["exit_code"], 2)

    def test_added_null_setting_still_requires_evidence(self):
        self.cand["json"]["effective_settings"]["NewFeature"] = None
        self.assertEqual(self.result()["exit_code"], 2)

    def test_unmatched_controls_make_quality_not_comparable(self):
        self.cand["input_controls_sha256"] = "f" * 64
        self.cand["json"]["input_controls_sha256"] = "f" * 64
        report = self.result()
        self.assertEqual(report["exit_code"], 2)
        self.assertTrue(all(v == "NOT-COMPARABLE" for v in report["results"].values()))

    def test_serialized_artifacts_load_and_preserve_control_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "run.json").write_text(json.dumps(self.base["json"]), encoding="utf-8")
            (root / "dispatch_controls.bin").write_bytes(self.base["controls"])
            for name, rows in (("metrics.csv", self.base["metrics"]),
                               ("temporal.csv", self.base["temporal"])):
                with (root / name).open("w", newline="", encoding="utf-8") as stream:
                    writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
                    writer.writeheader()
                    writer.writerows(rows)
            loaded = compare.load_run(root)
            self.assertEqual(compare.analyze(loaded, loaded)["exit_code"], 0)
            data = loaded["json"]
            data["input_controls_sha256"] = "f" * 64
            with self.assertRaises(compare.EvidenceError):
                compare.analyze(loaded, loaded)

    def test_excluded_lobe_row_count_drift_is_still_reported(self):
        self.cand["metrics"] = [r for r in self.cand["metrics"] if r["stage"] != "lobe_diffuse"]
        self.assertEqual(self.result()["exit_code"], 2)

    def test_invalid_tolerances_do_not_turn_off_guards(self):
        for bounds in ({"rmse_tolerance": float("nan")}, {"detail_band": -1},
                       {"temporal_tolerance": 0.5}, {"energy_band": float("inf")}):
            with self.subTest(bounds=bounds), self.assertRaises(compare.EvidenceError):
                self.result(bounds=bounds)


class RunnerArgumentTests(unittest.TestCase):
    @unittest.skipUnless(os.name == "nt", "run.cmd requires Windows cmd.exe")
    def test_baseline_only_without_source_fails_before_build_or_pass(self):
        runner = Path(__file__).with_name("run.cmd").resolve()
        with tempfile.TemporaryDirectory() as temp:
            for extra in ([], ["--build-only"]):
                with self.subTest(extra=extra):
                    out = Path(temp) / "must-not-be-created"
                    result = subprocess.run(
                        [os.environ.get("COMSPEC", "cmd.exe"), "/d", "/c", str(runner),
                         "gpu", str(out), "--baseline-only", *extra],
                        capture_output=True, text=True, check=False)
                    self.assertNotEqual(result.returncode, 0)
                    output = result.stdout + result.stderr
                    self.assertIn("--baseline-only requires --baseline-source", output)
                    self.assertNotIn("PASS", output)
                    self.assertFalse(out.exists())

    def test_runner_clears_inherited_feature_environment_variables(self):
        runner = Path(__file__).with_name("run.cmd").read_text(encoding="utf-8")
        head = runner.split(":ParseFlags", 1)[0]
        for variable in ("FEATURE_CASE", "ZERO_REFERENCE", "SET_VALUES"):
            self.assertIn(f'set "{variable}="', head)


if __name__ == "__main__":
    unittest.main()
