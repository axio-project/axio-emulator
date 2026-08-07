#!/usr/bin/env python3

from __future__ import annotations

import copy
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

from analyze_completion_interval import analyze_runs


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
ANALYZER = SCRIPT_DIR / "analyze_completion_interval.py"


def load_seed() -> dict:
    path = SCRIPT_DIR / "fixtures" / "valid-window.jsonl"
    return json.loads(path.read_text(encoding="utf-8"))


def load_accuracy_run_seeds() -> list[dict]:
    path = SCRIPT_DIR / "fixtures" / "accuracy-runs.jsonl"
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def make_run(
    *,
    interval_ns: float = 25.0,
    successful_rate_mpps: float | None = None,
    completion_rate_mpps: float | None = None,
    invalid_warmup: bool = False,
) -> list[dict]:
    if successful_rate_mpps is None:
        successful_rate_mpps = 1000.0 / interval_ns
    if completion_rate_mpps is None:
        completion_rate_mpps = 1000.0 / interval_ns
    records = []
    for window_id in range(30):
        record = copy.deepcopy(load_seed())
        record["window_id"] = window_id
        record["throughput"]["e2e_mpps"] = successful_rate_mpps
        record["latency"] = {"p50_us": 1.0, "p99_us": 1.5, "p999_us": 2.0}
        nic_rx = record["stages"]["nic_rx"]
        nic_rx["throughput_mpps"] = completion_rate_mpps
        interval_cycles = interval_ns * 3.0
        nic_rx["completion_interval_cycles"] = interval_cycles
        nic_rx["completion_interval_ns"] = interval_ns
        nic_rx["slowest_interval_cycles"] = interval_cycles
        nic_rx["capacity_interval_cycles"] = interval_cycles
        records.append(record)

    if invalid_warmup:
        nic_rx = records[0]["stages"]["nic_rx"]
        nic_rx["throughput_mpps"] = None
        for key in ("completion_interval_cycles", "completion_interval_ns",
                    "slowest_interval_cycles", "capacity_interval_cycles"):
            nic_rx[key] = None
    return records


def stable_runs() -> dict[str, list[dict]]:
    runs = {}
    for seed in load_accuracy_run_seeds():
        run_id = seed.pop("run_id")
        runs[run_id] = make_run(**seed)
    return runs


def write_run(path: pathlib.Path, records: list[dict]) -> None:
    path.write_text(
        "".join(json.dumps(record, separators=(",", ":")) + "\n"
                for record in records),
        encoding="utf-8",
    )


class AnalyzeCompletionIntervalTest(unittest.TestCase):
    def test_three_stable_runs_pass_all_gates(self) -> None:
        summary = analyze_runs(stable_runs())
        self.assertTrue(summary["gate_passed"])
        self.assertEqual(summary["run_count"], 3)
        self.assertEqual(summary["accepted_window_count"], 60)
        self.assertEqual(summary["excluded_warmup_window_count"], 30)
        self.assertEqual(summary["rejected_window_count"], 0)
        self.assertEqual(summary["completion_error_window_count"], 0)
        self.assertAlmostEqual(summary["median_rate_relative_error"], 0.0)
        self.assertAlmostEqual(summary["interval_run_median_cv"], 0.0)
        self.assertAlmostEqual(summary["throughput_median_mpps"], 40.0)
        self.assertAlmostEqual(summary["latency_p999_median_us"], 2.0)

    def test_excessive_rate_error_fails(self) -> None:
        runs = {
            run_id: make_run(interval_ns=1000.0 / 38.0,
                             successful_rate_mpps=40.0)
            for run_id in ("run-a", "run-b", "run-c")
        }
        summary = analyze_runs(runs)
        self.assertFalse(summary["gate_passed"])
        self.assertGreater(summary["median_rate_relative_error"], 0.02)
        self.assertIn("rate_error", summary["failed_gates"])

    def test_one_inaccurate_cold_start_cannot_be_hidden_by_run_median(self) -> None:
        runs = {
            "run-a": make_run(interval_ns=25.0),
            "run-b": make_run(interval_ns=25.0),
            "run-c": make_run(interval_ns=23.0,
                              successful_rate_mpps=40.0),
        }
        summary = analyze_runs(runs)
        self.assertFalse(summary["gate_passed"])
        self.assertIn("rate_error", summary["failed_gates"])
        self.assertEqual(summary["rate_error_run_ids"], ["run-c"])
        self.assertGreater(
            summary["runs"][2]["median_rate_relative_error"], 0.02)

    def test_excessive_repeated_run_cv_fails(self) -> None:
        runs = {
            "run-a": make_run(interval_ns=20.0),
            "run-b": make_run(interval_ns=25.0),
            "run-c": make_run(interval_ns=30.0),
        }
        summary = analyze_runs(runs)
        self.assertFalse(summary["gate_passed"])
        self.assertGreater(summary["interval_run_median_cv"], 0.05)
        self.assertIn("interval_cv", summary["failed_gates"])

    def test_completion_error_is_reported_and_fails(self) -> None:
        runs = stable_runs()
        runs["run-a"][15]["counters"]["nic_rx_completion_error_count"] = 1
        summary = analyze_runs(runs)
        self.assertFalse(summary["gate_passed"])
        self.assertEqual(summary["completion_error_window_count"], 1)
        self.assertIn("completion_error", summary["failed_gates"])

    def test_null_final_interval_is_not_silently_discarded(self) -> None:
        runs = stable_runs()
        nic_rx = runs["run-a"][10]["stages"]["nic_rx"]
        nic_rx["completion_interval_ns"] = None
        nic_rx["throughput_mpps"] = None
        summary = analyze_runs(runs)
        self.assertFalse(summary["gate_passed"])
        self.assertEqual(summary["rejected_window_count"], 1)
        self.assertIn("invalid_sample_window", summary["failed_gates"])

    def test_null_e2e_throughput_is_rejected_as_an_invalid_window(self) -> None:
        runs = stable_runs()
        runs["run-a"][10]["throughput"]["e2e_mpps"] = None
        summary = analyze_runs(runs)
        self.assertFalse(summary["gate_passed"])
        self.assertEqual(summary["rejected_window_count"], 1)
        self.assertIn("invalid_sample_window", summary["failed_gates"])

    def test_invalid_warmup_is_reported_but_does_not_fail(self) -> None:
        runs = stable_runs()
        runs["run-a"] = make_run(invalid_warmup=True)
        summary = analyze_runs(runs)
        self.assertTrue(summary["gate_passed"])
        self.assertEqual(summary["invalid_warmup_window_count"], 1)
        self.assertEqual(summary["accepted_window_count"], 60)

    def test_cli_rejects_nonfinite_input_with_canonical_json(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "nonfinite.jsonl"
            path.write_text('{"value":NaN}\n', encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(ANALYZER), str(path)],
                capture_output=True, text=True, check=False,
            )
        self.assertNotEqual(result.returncode, 0)
        parsed = json.loads(result.stdout)
        self.assertFalse(parsed["gate_passed"])
        self.assertTrue(parsed["input_errors"])
        canonical = json.dumps(parsed, sort_keys=True, separators=(",", ":"))
        self.assertEqual(result.stdout, canonical + "\n")

    def test_cli_accepts_one_file_per_cold_start(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for run_id in ("run-a", "run-b", "run-c"):
                path = pathlib.Path(directory) / f"{run_id}.jsonl"
                write_run(path, make_run())
                paths.append(path)
            result = subprocess.run(
                [sys.executable, str(ANALYZER), *(str(path) for path in paths)],
                capture_output=True, text=True, check=False,
            )
        self.assertEqual(result.returncode, 0)
        parsed = json.loads(result.stdout)
        self.assertTrue(parsed["gate_passed"])
        canonical = json.dumps(parsed, sort_keys=True, separators=(",", ":"))
        self.assertEqual(result.stdout, canonical + "\n")

    def test_cli_returns_nonzero_when_an_accuracy_gate_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for run_id in ("run-a", "run-b", "run-c"):
                path = pathlib.Path(directory) / f"{run_id}.jsonl"
                write_run(path, make_run(interval_ns=1000.0 / 38.0,
                                         successful_rate_mpps=40.0))
                paths.append(path)
            result = subprocess.run(
                [sys.executable, str(ANALYZER), *(str(path) for path in paths)],
                capture_output=True, text=True, check=False,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("rate_error", json.loads(result.stdout)["failed_gates"])


if __name__ == "__main__":
    unittest.main()
