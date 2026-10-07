"""Reject incomplete measurement evidence and preserve repeat ranges."""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from pathlib import Path
from contextlib import redirect_stdout
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import run_telemetry_baseline as baseline  # noqa: E402


def report(wall_ns: int = 100) -> dict[str, str]:
    return {
        "scenario": "supported-mix",
        "profiler": "stage-timers",
        "wall_ns": str(wall_ns),
        "process_cpu_ns": "95",
        "receive_wait": "10",
        "enqueue_to_process": "20",
        "dequeue_to_process": "15",
        "decode": "30",
        "diagnostics": "40",
        "publication": "50",
        "notification_evaluation": "60",
        "notification_dispatch": "70",
        "action_sink": "80",
        "callback_delivery": "90",
    }


class BaselineReportTests(unittest.TestCase):
    def test_parse_keeps_reports_separate_from_doctest_output(self) -> None:
        raw = (
            "[doctest] test cases: 2 | 2 passed\n"
            "telemetry_baseline scenario=silence profiler=stage-timers wall_ns=100 process_cpu_ns=90\n"
            "  stage_ns decode=3 publication=5\n"
            "telemetry_baseline scenario=supported-mix profiler=stage-timers wall_ns=200 process_cpu_ns=180\n"
            "  stage_ns decode=7 publication=11\n"
        )
        parsed = baseline.parse_reports(raw)
        self.assertEqual(len(parsed), 2)
        self.assertEqual(parsed[0]["decode"], "3")
        self.assertEqual(parsed[1]["decode"], "7")
        self.assertEqual(parsed[1]["scenario"], "supported-mix")

    def test_summary_records_actual_repeat_variance(self) -> None:
        summary = baseline.summarize([report(300), report(100), report(200)])
        self.assertIn("samples=3", summary)
        self.assertIn("wall_ns[min/median/max]=100/200/300", summary)

    def test_even_repeat_count_uses_the_midpoint_median(self) -> None:
        summary = baseline.summarize([report(300), report(100)])
        self.assertIn("wall_ns[min/median/max]=100/200.0/300", summary)

    def test_summary_rejects_no_reports(self) -> None:
        with self.assertRaises(ValueError):
            baseline.summarize([])

    def test_summary_rejects_missing_stage_instead_of_reporting_zero(self) -> None:
        incomplete = report()
        del incomplete["decode"]
        with self.assertRaises(ValueError):
            baseline.summarize([incomplete])

    def test_summary_rejects_non_numeric_measurements(self) -> None:
        invalid = report()
        invalid["decode"] = "unavailable"
        with self.assertRaises(ValueError):
            baseline.summarize([invalid])

    def test_summary_rejects_negative_measurements(self) -> None:
        invalid = report()
        invalid["wall_ns"] = "-1"
        with self.assertRaises(ValueError):
            baseline.summarize([invalid])

    def test_runner_rejects_a_repeat_without_reports(self) -> None:
        valid = (
            "telemetry_baseline scenario=supported-mix profiler=stage-timers wall_ns=100 "
            "process_cpu_ns=95\n"
            "  stage_ns receive_wait=10 enqueue_to_process=20 dequeue_to_process=15 "
            "decode=30 diagnostics=40 "
            "publication=50 notification_evaluation=60 notification_dispatch=70 "
            "action_sink=80 callback_delivery=90\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "tests" / "host" / "telemetry_baseline_tests"
            executable.parent.mkdir(parents=True)
            executable.touch()
            arguments = [
                "baseline", "--skip-build", "--build-dir", directory, "--repeats", "2"
            ]
            with patch.object(sys, "argv", arguments), patch.object(
                baseline, "run", side_effect=[valid, "[doctest] test cases: 0\n"]
            ), redirect_stdout(io.StringIO()), self.assertRaisesRegex(
                SystemExit, "release repeat 2 produced no telemetry_baseline reports"
            ):
                baseline.main()


if __name__ == "__main__":
    unittest.main()
