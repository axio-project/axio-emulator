#!/usr/bin/env python3
"""Strict validator for the compact Axio per-window JSONL contract."""

from __future__ import annotations

import copy
import json
import math
import pathlib
import sys
from typing import Any, Callable, Iterable


class SchemaError(ValueError):
    pass


TOP_LEVEL_KEYS = {
    "window_id", "throughput", "latency", "stages", "counters",
}
STAGE_KEYS = {
    "completion_time_per_packet_us", "stall_time_per_packet_us",
}
COUNTER_KEYS = {
    "app_enqueue_drop_count", "dispatcher_enqueue_drop_count",
    "nic_rx_completion_error_count",
}


def require(condition: bool, location: str, message: str) -> None:
    if not condition:
        raise SchemaError(f"{location}: {message}")


def require_object(value: Any, location: str) -> dict[str, Any]:
    require(isinstance(value, dict), location, "must be an object")
    return value


def require_exact_keys(
    value: dict[str, Any], expected: set[str], location: str
) -> None:
    missing = sorted(expected - value.keys())
    unknown = sorted(value.keys() - expected)
    require(not missing, location, f"missing required keys {missing}")
    require(not unknown, location, f"unknown keys {unknown}")


def require_uint(value: Any, location: str) -> int:
    require(type(value) is int and value >= 0,
            location, "must be a non-negative integer")
    return value


def require_number(value: Any, location: str, *, positive: bool = False) -> float:
    require(type(value) in (int, float), location, "must be a number")
    number = float(value)
    require(math.isfinite(number), location, "must be finite")
    require(number > 0 if positive else number >= 0,
            location, "must be positive" if positive else "must be non-negative")
    return number


def require_metric(value: Any, location: str) -> float | None:
    if value is None:
        return None
    return require_number(value, location)


def validate_stage(value: Any, location: str) -> None:
    stage = require_object(value, location)
    require_exact_keys(stage, STAGE_KEYS, location)
    require_metric(stage["completion_time_per_packet_us"],
                   f"{location}.completion_time_per_packet_us")
    require_metric(stage["stall_time_per_packet_us"],
                   f"{location}.stall_time_per_packet_us")


def validate_record(value: Any, location: str) -> int:
    record = require_object(value, location)
    require_exact_keys(record, TOP_LEVEL_KEYS, location)
    window_id = require_uint(record["window_id"], f"{location}.window_id")

    throughput = require_object(record["throughput"], f"{location}.throughput")
    require_exact_keys(throughput, {"e2e_mpps"}, f"{location}.throughput")
    require_metric(throughput["e2e_mpps"], f"{location}.throughput.e2e_mpps")

    latency = require_object(record["latency"], f"{location}.latency")
    require_exact_keys(latency, {"p50_us", "p99_us", "p999_us"},
                       f"{location}.latency")
    for key in ("p50_us", "p99_us", "p999_us"):
        require_metric(latency[key], f"{location}.latency.{key}")

    stages = require_object(record["stages"], f"{location}.stages")
    require_exact_keys(stages, {"app_tx", "nic_rx"}, f"{location}.stages")
    validate_stage(stages["app_tx"], f"{location}.stages.app_tx")
    nic_rx = require_object(stages["nic_rx"], f"{location}.stages.nic_rx")
    nic_rx_keys = {"throughput_mpps", "completion_interval_cycles",
                   "completion_interval_ns", "slowest_interval_cycles",
                   "capacity_interval_cycles"}
    require_exact_keys(nic_rx, nic_rx_keys, f"{location}.stages.nic_rx")
    require_metric(nic_rx["throughput_mpps"],
                   f"{location}.stages.nic_rx.throughput_mpps")
    interval_keys = ("completion_interval_cycles", "completion_interval_ns",
                     "slowest_interval_cycles", "capacity_interval_cycles")
    interval_values = [
        require_metric(nic_rx[key], f"{location}.stages.nic_rx.{key}")
        for key in interval_keys
    ]
    available_intervals = [value for value in interval_values if value is not None]
    require(not available_intervals or
            (len(available_intervals) == len(interval_values) and
             all(value > 0 for value in available_intervals)),
            location, "NIC RX intervals must be all positive or all null")

    counters = require_object(record["counters"], f"{location}.counters")
    require_exact_keys(counters, COUNTER_KEYS, f"{location}.counters")
    for key in COUNTER_KEYS:
        require_uint(counters[key], f"{location}.counters.{key}")
    return window_id


def reject_nonfinite(value: str) -> None:
    raise SchemaError(f"non-finite JSON constant {value}")


def parse_line(line: str, location: str) -> dict[str, Any]:
    try:
        value = json.loads(line, parse_constant=reject_nonfinite)
    except (json.JSONDecodeError, SchemaError) as error:
        raise SchemaError(f"{location}: invalid JSON: {error}") from error
    return require_object(value, location)


def validate_records(records: Iterable[tuple[dict[str, Any], str]]) -> int:
    seen: set[int] = set()
    last_window: int | None = None
    count = 0
    for record, location in records:
        window_id = validate_record(record, location)
        require(window_id not in seen, location, "duplicate window ID")
        if last_window is not None:
            require(window_id > last_window, location,
                    "window IDs must increase monotonically")
        seen.add(window_id)
        last_window = window_id
        count += 1
    return count


def load_path(path: pathlib.Path) -> list[tuple[dict[str, Any], str]]:
    records: list[tuple[dict[str, Any], str]] = []
    with path.open(encoding="utf-8") as source:
        for line_number, raw_line in enumerate(source, start=1):
            line = raw_line.rstrip("\n")
            require(bool(line), f"{path}:{line_number}", "blank JSONL lines are forbidden")
            location = f"{path}:{line_number}"
            records.append((parse_line(line, location), location))
    require(bool(records), str(path), "must contain at least one record")
    return records


def expect_rejected(name: str, operation: Callable[[], None]) -> None:
    try:
        operation()
    except SchemaError:
        return
    raise RuntimeError(f"schema contract mutation was accepted: {name}")


def run_contract_mutations(script_dir: pathlib.Path) -> None:
    valid = load_path(script_dir / "fixtures" / "valid-window.jsonl")[0][0]
    invalid = load_path(script_dir / "fixtures" / "null-nic-window.jsonl")[0][0]

    def rejects_record(name: str, value: dict[str, Any]) -> None:
        expect_rejected(name, lambda: validate_record(value, name))

    mutation = copy.deepcopy(valid)
    del mutation["throughput"]
    rejects_record("missing key", mutation)
    mutation = copy.deepcopy(valid)
    mutation["unknown"] = 1
    rejects_record("unknown key", mutation)
    mutation = copy.deepcopy(valid)
    mutation["window_id"] = True
    rejects_record("wrong primitive type", mutation)
    expect_rejected("non-finite input",
                    lambda: parse_line('{"value":NaN}', "non-finite input"))
    mutation = copy.deepcopy(invalid)
    mutation["stages"]["nic_rx"]["completion_interval_cycles"] = 0.0
    rejects_record("unavailable encoded as zero", mutation)
    mutation = copy.deepcopy(invalid)
    mutation["identity"] = {"role": "client"}
    rejects_record("identity metadata is not part of compact metrics", mutation)
    expect_rejected(
        "duplicate window ID",
        lambda: validate_records([(copy.deepcopy(valid), "first"),
                                  (copy.deepcopy(valid), "duplicate")]),
    )
    later = copy.deepcopy(valid)
    later["window_id"] = 1
    earlier = copy.deepcopy(valid)
    expect_rejected(
        "non-monotonic window IDs",
        lambda: validate_records([(later, "later"), (earlier, "earlier")]),
    )


def main(argv: list[str]) -> int:
    require(bool(argv), "command line", "usage: metrics_schema_test.py PATH...")
    script_dir = pathlib.Path(__file__).resolve().parent
    run_contract_mutations(script_dir)
    records: list[tuple[dict[str, Any], str]] = []
    for argument in argv:
        records.extend(load_path(pathlib.Path(argument)))
    count = validate_records(records)
    print(f"Axio metrics schema validation passed: {count} records")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (OSError, SchemaError, RuntimeError) as error:
        print(f"Axio metrics schema validation failed: {error}", file=sys.stderr)
        raise SystemExit(1)
