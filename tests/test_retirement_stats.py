"""Meaningful parser checks for incomplete captures and retirement edge states."""

import importlib.util
from pathlib import Path
import sys
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "exports" / "retirement_stats.py"
SPEC = importlib.util.spec_from_file_location("retirement_stats", SCRIPT)
stats = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = stats
SPEC.loader.exec_module(stats)


def row(capture=10, attempt=1, **changes):
    data = dict(
        capture=capture, t_ms=1, source="Record", attempt=attempt, pending=123,
        submitted=1, passes=1, accepted=0, native=0, done0=9, job0=10, done1=0, job1=0,
        done2=0, job2=0, gpu_before=5, gpu_after=5, target=6, retired=0,
        wait_ms=0, extra_wait_ms=0, extra_gpu_before=0, extra_gpu_after=0,
        extra_target=0, extra_waited=0, outcome="pending_skip", width=1280,
        height=720, every_frame=0, recorded_at=100, submitted_at=101,
    )
    data.update(changes)
    return "[AMD-S4] " + " ".join(f"{key}={value}" for key, value in data.items())


def capture(capture_id, rows, reason="window", declared=None):
    return [
        f"[AMD-S4-BEGIN] capture={capture_id} runtime=0.3.0 window_ms=30000 schema=1",
        *rows,
        f"[AMD-S4-END] capture={capture_id} rows={len(rows) if declared is None else declared} "
        f"reason={reason} elapsed_ms=30000",
    ]


def analyze(rows):
    captures, warnings = stats.parse_captures(capture(10, rows))
    assert not warnings
    selected = stats.select_capture(captures)
    return selected, stats.summarize(selected)


class RetirementStatsTests(unittest.TestCase):
    def test_last_complete_and_explicit_capture_with_truncated_tail(self):
        lines = capture(10, [row()]) + capture(20, [row(capture=20)])
        lines += ["[AMD-S4-BEGIN] capture=30 runtime=0.3.0 window_ms=30000 schema=1", row(capture=30)]
        captures, warnings = stats.parse_captures(lines)
        self.assertEqual(stats.select_capture(captures).capture_id, 20)
        self.assertEqual(stats.select_capture(captures, 10).capture_id, 10)
        self.assertIn("truncated", captures[-1].warnings[0])
        with self.assertRaises(ValueError):
            stats.select_capture(captures, 30)

    def test_independent_blockers_and_unassigned_target(self):
        _, report = analyze([
            row(attempt=1),  # BOTH native and fence unfinished.
            row(attempt=2, submitted=0, gpu_after=6),
            row(attempt=3, native=1),
            row(attempt=4, native=1, passes=0),
            row(attempt=5, native=1, gpu_after=stats.UINT64_MAX),
        ])
        matrix = report["blockers"]
        self.assertEqual(matrix[("submitted", "native_pending", "fence_pending")], 1)
        self.assertEqual(matrix[("unsubmitted", "native_pending", "fence_unassigned")], 1)
        self.assertEqual(matrix[("submitted", "native_done", "fence_pending")], 1)
        self.assertEqual(matrix[("submitted", "no_native_work", "fence_pending")], 1)
        self.assertEqual(matrix[("submitted", "native_done", "device_removed")], 1)

    def test_waiting_then_success_and_second_wait_are_not_lost(self):
        _, report = analyze([
            row(attempt=1, native=1, gpu_after=6, wait_ms=2, retired=1, outcome="recorded"),
            row(attempt=2, pending=0, outcome="recorded", extra_waited=1, extra_wait_ms=4),
            row(attempt=3, wait_ms=16, outcome="fence_skip"),
        ])
        self.assertEqual(report["waits"]["recorded"]["mean"], 3)
        self.assertEqual(report["waits"]["recorded"]["max"], 4)
        self.assertEqual(report["waits"]["not recorded"]["mean"], 16)
        self.assertEqual(report["waits"]["all Record"]["over"][5], 1)
        self.assertEqual(report["waits"]["all Record"]["over"][16], 0)
        self.assertEqual(report["retired_during_record"], 1)

    def test_unsubmitted_admission_skip_is_counted_separately_from_full_slots(self):
        selected, report = analyze([
            row(attempt=1, outcome="unsubmitted_skip", submitted=0, submitted_at=0),
            row(attempt=2, outcome="pending_skip"),
            row(attempt=3, outcome="recorded", accepted=1),
        ])
        self.assertFalse(selected.warnings)
        self.assertEqual(len(report["records"]), 3)
        self.assertEqual(report["outcomes"]["unsubmitted_skip"], 1)
        self.assertEqual(report["blockers"], {
            ("submitted", "native_pending", "fence_pending"): 1,
        })
        self.assertEqual(report["waits"]["not recorded"]["n"], 2)
        self.assertIn("unsubmitted_skip=1 (33.3%)", stats.render_report(selected, report))

    def test_status_observations_and_duplicates_do_not_inflate_record_counts(self):
        selected, report = analyze([
            row(attempt=1), row(attempt=1),
            row(attempt=1, source="Status", outcome="poll", retired=1, wait_ms=10),
            row(attempt=2, outcome="recorded", pending=0),
        ])
        self.assertEqual(len(report["records"]), 2)
        self.assertEqual(report["duplicate_attempts"], {1: 1})
        self.assertEqual(report["sources"], {"Status": 1})
        self.assertEqual(report["waits"]["all Record"]["max"], 0)
        self.assertIn("duplicate Record attempts", stats.render_report(selected, report))

    def test_recorded_output_does_not_imply_native_acceptance(self):
        _, report = analyze([
            row(attempt=1, outcome="recorded", accepted=0),
            row(attempt=2, outcome="recorded", accepted=1),
            # A can accept work before a later B error prevents returning output.
            row(attempt=3, outcome="other_skip", accepted=2),
        ])
        self.assertEqual(report["outcomes"]["recorded"], 2)
        self.assertEqual(report["accepted_records"], 2)
        self.assertEqual(report["accepted_passes"], {0: 1, 1: 1, 2: 1})
        captures, _ = stats.parse_captures(capture(10, [row(accepted=4)]))
        self.assertEqual(len(captures[0].rows), 0)
        self.assertTrue(captures[0].warnings)

    def test_bad_rows_and_row_count_mismatch_are_visible(self):
        bad = row(attempt=2).replace("native=0", "native=oops")
        missing = row(attempt=3).replace(" gpu_after=5", "")
        lines = capture(10, [row(), bad, missing], declared=4)
        captures, _ = stats.parse_captures(lines)
        selected = stats.select_capture(captures)
        self.assertEqual(selected.raw_rows, 3)
        self.assertEqual(len(selected.rows), 1)
        self.assertEqual(len(selected.warnings), 3)
        self.assertIn("END.rows", selected.warnings[-1])

    def test_capacity_finite_numbers_and_duplicate_keys(self):
        lines = capture(10, [
            row(wait_ms="nan"), row(attempt=2) + " native=1", row(attempt=3),
        ], reason="capacity")
        captures, _ = stats.parse_captures(lines)
        selected = stats.select_capture(captures)
        self.assertEqual(len(selected.rows), 1)
        self.assertTrue(any("capacity" in warning for warning in selected.warnings))
        self.assertTrue(any("finite" in warning for warning in selected.warnings))
        self.assertTrue(any("duplicate field" in warning for warning in selected.warnings))

    def test_mismatched_end_cannot_complete_another_capture(self):
        lines = capture(10, [row()])[:-1]
        lines += ["[AMD-S4-END] capture=99 rows=1 reason=window elapsed_ms=30000"]
        captures, warnings = stats.parse_captures(lines)
        with self.assertRaises(ValueError):
            stats.select_capture(captures)
        self.assertTrue(any("mismatched" in warning for warning in warnings))


if __name__ == "__main__":
    unittest.main()
