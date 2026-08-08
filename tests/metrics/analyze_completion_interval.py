#!/usr/bin/env python3
"""Evaluate Axio NIC RX completion interval accuracy across cold starts."""

from __future__ import annotations

import json
import math
import pathlib
import statistics
import sys
from typing import Any, Iterable


SCHEMA = "axio.nic_rx_completion_accuracy/v1"
WARMUP_WINDOWS = 10
SAMPLE_WINDOWS = 20
MAX_RATE_RELATIVE_ERROR = 0.02
MAX_INTERVAL_CV = 0.05
MIN_RUNS = 3


class AnalysisError(ValueError):
    pass


def finite_number(value: Any, location: str, *, positive: bool = False) -> float:
    if type(value) not in (int, float):
        raise AnalysisError(f"{location}: expected a number")
    number = float(value)
    if not math.isfinite(number):
        raise AnalysisError(f"{location}: expected a finite number")
    if positive and number <= 0:
        raise AnalysisError(f"{location}: expected a positive number")
    return number


def nonnegative_integer(value: Any, location: str) -> int:
    if type(value) is not int or value < 0:
        raise AnalysisError(f"{location}: expected a non-negative integer")
    return value


def nested(record: dict[str, Any], path: tuple[str, ...], location: str) -> Any:
    value: Any = record
    for key in path:
        if not isinstance(value, dict) or key not in value:
            raise AnalysisError(f"{location}: missing {'.'.join(path)}")
        value = value[key]
    return value


def window_is_valid(record: dict[str, Any]) -> bool:
    interval = nested(
        record, ("stages", "nic_rx", "completion_interval_ns"), "record"
    )
    if interval is None:
        return False
    finite_number(interval, "record.stages.nic_rx.completion_interval_ns",
                  positive=True)
    completion_rate = nested(
        record, ("stages", "nic_rx", "throughput_mpps"), "record"
    )
    if completion_rate is None:
        return False
    finite_number(completion_rate, "record.stages.nic_rx.throughput_mpps",
                  positive=True)
    e2e_rate = nested(record, ("throughput", "e2e_mpps"), "record")
    if e2e_rate is None:
        return False
    finite_number(e2e_rate, "record.throughput.e2e_mpps", positive=True)
    return True


def completion_error_count(record: dict[str, Any], location: str) -> int:
    counters = nested(record, ("counters",), location)
    return nonnegative_integer(
        counters.get("nic_rx_completion_error_count"),
        f"{location}.counters.nic_rx_completion_error_count",
    )


def median_or_none(values: list[float]) -> float | None:
    return statistics.median(values) if values else None


def analyze_runs(
    runs: dict[str, list[dict[str, Any]]],
    *,
    warmup_windows: int = WARMUP_WINDOWS,
    sample_windows: int = SAMPLE_WINDOWS,
) -> dict[str, Any]:
    for run_id, records in runs.items():
        if not isinstance(run_id, str) or not run_id:
            raise AnalysisError("run ID must be a non-empty string")
        if not isinstance(records, list) or not records:
            raise AnalysisError(f"{run_id}: expected a non-empty record list")
        for index, record in enumerate(records):
            if not isinstance(record, dict):
                raise AnalysisError(f"{run_id}.record[{index}]: expected an object")

    failed_gates: set[str] = set()
    if len(runs) < MIN_RUNS:
        failed_gates.add("insufficient_runs")
    run_summaries: list[dict[str, Any]] = []
    total_accepted = 0
    total_rejected = 0
    total_warmup = 0
    invalid_warmup_count = 0
    completion_error_windows = 0

    for run_id in sorted(runs):
        run_records = runs[run_id]
        by_window: dict[int, dict[str, Any]] = {}
        for record in run_records:
            window_id = nonnegative_integer(record.get("window_id"),
                                            f"{run_id}.window_id")
            if window_id in by_window:
                failed_gates.add("run_shape")
            by_window[window_id] = record
        expected_ids = list(range(warmup_windows + sample_windows))
        if sorted(by_window) != expected_ids:
            failed_gates.add("run_shape")

        ordered = [by_window[window_id] for window_id in expected_ids
                   if window_id in by_window]
        warmup = [record for record in ordered
                  if record["window_id"] < warmup_windows]
        samples = [record for record in ordered
                   if warmup_windows <= record["window_id"] <
                   warmup_windows + sample_windows]
        total_warmup += len(warmup)
        run_invalid_warmup = sum(not window_is_valid(record)
                                 for record in warmup)
        invalid_warmup_count += run_invalid_warmup

        intervals: list[float] = []
        rate_errors: list[float] = []
        throughputs: list[float] = []
        p999_values: list[float] = []
        rejected_window_ids: list[int] = []
        run_completion_error_windows = 0
        for record in samples:
            window_id = record["window_id"]
            errors = completion_error_count(
                record, f"{run_id}.window[{window_id}]"
            )
            if errors != 0:
                run_completion_error_windows += 1
            if errors != 0 or not window_is_valid(record):
                rejected_window_ids.append(window_id)
                continue
            completion_rate = finite_number(
                nested(record, ("stages", "nic_rx", "throughput_mpps"),
                       run_id),
                f"{run_id}.window[{window_id}].nic_rx_throughput",
                positive=True,
            )
            interval = finite_number(
                nested(record,
                       ("stages", "nic_rx", "completion_interval_ns"),
                       run_id),
                f"{run_id}.window[{window_id}].completion_interval_ns",
                positive=True,
            )
            e2e_rate = finite_number(
                nested(record, ("throughput", "e2e_mpps"), run_id),
                f"{run_id}.window[{window_id}].throughput", positive=True,
            )
            p999 = nested(record, ("latency", "p999_us"), run_id)
            if p999 is not None:
                p999_values.append(finite_number(
                    p999, f"{run_id}.window[{window_id}].latency_p999",
                    positive=True))
            intervals.append(interval)
            rate_errors.append(abs(completion_rate - e2e_rate) / e2e_rate)
            throughputs.append(e2e_rate)

        accepted = len(intervals)
        rejected = sample_windows - accepted
        total_accepted += accepted
        total_rejected += rejected
        completion_error_windows += run_completion_error_windows
        if rejected != 0 or len(samples) != sample_windows:
            failed_gates.add("invalid_sample_window")
        run_summaries.append({
            "accepted_window_count": accepted,
            "invalid_warmup_window_count": run_invalid_warmup,
            "latency_p999_median_us": median_or_none(p999_values),
            "median_interval_ns": median_or_none(intervals),
            "median_rate_relative_error": median_or_none(rate_errors),
            "rejected_window_ids": sorted(set(rejected_window_ids)),
            "run_id": run_id,
            "throughput_median_mpps": median_or_none(throughputs),
        })

    run_rate_errors = [run["median_rate_relative_error"] for run in run_summaries
                       if run["median_rate_relative_error"] is not None]
    rate_error_run_ids = [
        run["run_id"] for run in run_summaries
        if run["median_rate_relative_error"] is None or
        run["median_rate_relative_error"] > MAX_RATE_RELATIVE_ERROR
    ]
    run_intervals = [run["median_interval_ns"] for run in run_summaries
                     if run["median_interval_ns"] is not None]
    run_throughputs = [run["throughput_median_mpps"] for run in run_summaries
                       if run["throughput_median_mpps"] is not None]
    run_p999 = [run["latency_p999_median_us"] for run in run_summaries
                if run["latency_p999_median_us"] is not None]
    median_rate_error = median_or_none(run_rate_errors)
    interval_cv = None
    if len(run_intervals) == len(run_summaries) and run_intervals:
        mean_interval = statistics.mean(run_intervals)
        interval_cv = (statistics.pstdev(run_intervals) / mean_interval
                       if mean_interval > 0 else None)
    if rate_error_run_ids:
        failed_gates.add("rate_error")
    if interval_cv is None or interval_cv > MAX_INTERVAL_CV:
        failed_gates.add("interval_cv")
    if completion_error_windows != 0:
        failed_gates.add("completion_error")

    return {
        "accepted_window_count": total_accepted,
        "completion_error_window_count": completion_error_windows,
        "excluded_warmup_window_count": total_warmup,
        "failed_gates": sorted(failed_gates),
        "gate_passed": not failed_gates,
        "input_errors": [],
        "interval_run_median_cv": interval_cv,
        "invalid_warmup_window_count": invalid_warmup_count,
        "latency_p999_median_us": median_or_none(run_p999),
        "median_rate_relative_error": median_rate_error,
        "rate_error_run_ids": rate_error_run_ids,
        "rejected_window_count": total_rejected,
        "run_count": len(run_summaries),
        "runs": run_summaries,
        "schema": SCHEMA,
        "thresholds": {
            "max_interval_run_median_cv": MAX_INTERVAL_CV,
            "max_rate_relative_error": MAX_RATE_RELATIVE_ERROR,
            "minimum_run_count": MIN_RUNS,
            "sample_windows": sample_windows,
            "warmup_windows": warmup_windows,
        },
        "throughput_median_mpps": median_or_none(run_throughputs),
    }


def reject_nonfinite(value: str) -> None:
    raise AnalysisError(f"non-finite JSON constant {value}")


def load_runs(paths: Iterable[pathlib.Path]) -> dict[str, list[dict[str, Any]]]:
    runs: dict[str, list[dict[str, Any]]] = {}
    for path in paths:
        run_id = path.stem
        if run_id in runs:
            raise AnalysisError(f"duplicate run filename stem: {run_id}")
        records: list[dict[str, Any]] = []
        with path.open(encoding="utf-8") as source:
            for line_number, raw_line in enumerate(source, start=1):
                line = raw_line.rstrip("\n")
                if not line:
                    raise AnalysisError(f"{path}:{line_number}: blank JSONL line")
                try:
                    record = json.loads(line, parse_constant=reject_nonfinite)
                except (json.JSONDecodeError, AnalysisError) as error:
                    raise AnalysisError(f"{path}:{line_number}: {error}") from error
                if not isinstance(record, dict):
                    raise AnalysisError(f"{path}:{line_number}: expected an object")
                records.append(record)
        if not records:
            raise AnalysisError(f"{path}: no metrics records supplied")
        runs[run_id] = records
    if not runs:
        raise AnalysisError("no run files supplied")
    return runs


def failure_summary(error: Exception) -> dict[str, Any]:
    return {
        "failed_gates": ["input_error"],
        "gate_passed": False,
        "input_errors": [str(error)],
        "schema": SCHEMA,
    }


def canonical_json(value: dict[str, Any]) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      allow_nan=False)


def main(argv: list[str]) -> int:
    try:
        if not argv:
            raise AnalysisError(
                "usage: analyze_completion_interval.py RUN.jsonl [RUN.jsonl ...]"
            )
        summary = analyze_runs(load_runs(pathlib.Path(arg) for arg in argv))
    except (AnalysisError, OSError, statistics.StatisticsError) as error:
        summary = failure_summary(error)
    print(canonical_json(summary))
    return 0 if summary["gate_passed"] else 1

if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
