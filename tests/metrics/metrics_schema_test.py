#!/usr/bin/env python3
"""Strict standard-library validator for the axio.metrics/v1 JSONL contract."""

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
    "schema", "run_id", "window_id", "identity", "window", "throughput",
    "latency", "stages", "counters", "queues",
}
IDENTITY_KEYS = {
    "role", "backend", "version", "git_commit", "build_fingerprint",
    "config_fingerprint",
}
WINDOW_KEYS = {
    "duration_seconds", "measurement_valid", "capacity_comparable",
    "invalid_reasons",
}
STAGE_KEYS = {
    "throughput_mpps", "completion_time_per_packet_us",
    "stall_time_per_packet_us",
}
QUEUE_KEYS = {
    "workspace_id", "workload_ids", "successful_completion_count",
    "timed_completion_count", "successful_poll_count", "empty_poll_count",
    "completion_error_count", "first_completion_tsc", "last_completion_tsc",
    "tsc_frequency_ghz", "clock_valid", "measurement_valid",
    "capacity_comparable", "invalid_reasons", "completion_interval_cycles",
    "completion_interval_ns", "completion_rate_mpps",
}
COUNTER_KEYS = {
    "app_enqueue_drop_count", "dispatcher_enqueue_drop_count",
    "nic_tx_packet_count", "nic_rx_successful_completion_count",
    "nic_rx_timed_completion_count", "nic_rx_completion_error_count",
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


def require_bool(value: Any, location: str) -> bool:
    require(type(value) is bool, location, "must be a boolean")
    return value


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


def require_string(value: Any, location: str) -> str:
    require(isinstance(value, str) and bool(value),
            location, "must be a non-empty string")
    return value


def require_string_list(value: Any, location: str) -> list[str]:
    require(isinstance(value, list), location, "must be an array")
    for index, item in enumerate(value):
        require_string(item, f"{location}[{index}]")
    return value


def validate_stage(value: Any, location: str) -> None:
    stage = require_object(value, location)
    require_exact_keys(stage, STAGE_KEYS, location)
    require_metric(stage["throughput_mpps"], f"{location}.throughput_mpps")
    require_metric(stage["completion_time_per_packet_us"],
                   f"{location}.completion_time_per_packet_us")
    require_metric(stage["stall_time_per_packet_us"],
                   f"{location}.stall_time_per_packet_us")


def validate_queue(value: Any, location: str) -> None:
    queue = require_object(value, location)
    require_exact_keys(queue, QUEUE_KEYS, location)
    require_uint(queue["workspace_id"], f"{location}.workspace_id")
    workload_ids = queue["workload_ids"]
    require(isinstance(workload_ids, list),
            f"{location}.workload_ids", "must be an array")
    for index, workload_id in enumerate(workload_ids):
        require_uint(workload_id, f"{location}.workload_ids[{index}]")
    for key in (
        "successful_completion_count", "timed_completion_count",
        "successful_poll_count", "empty_poll_count", "completion_error_count",
        "first_completion_tsc", "last_completion_tsc",
    ):
        require_uint(queue[key], f"{location}.{key}")
    frequency = require_number(queue["tsc_frequency_ghz"],
                               f"{location}.tsc_frequency_ghz", positive=True)
    clock_valid = require_bool(queue["clock_valid"],
                               f"{location}.clock_valid")
    measurement_valid = require_bool(queue["measurement_valid"],
                                     f"{location}.measurement_valid")
    require_bool(queue["capacity_comparable"],
                 f"{location}.capacity_comparable")
    reasons = require_string_list(queue["invalid_reasons"],
                                  f"{location}.invalid_reasons")
    interval_cycles = require_metric(queue["completion_interval_cycles"],
                                     f"{location}.completion_interval_cycles")
    interval_ns = require_metric(queue["completion_interval_ns"],
                                 f"{location}.completion_interval_ns")
    rate_mpps = require_metric(queue["completion_rate_mpps"],
                               f"{location}.completion_rate_mpps")

    if measurement_valid:
        require(clock_valid, location, "valid measurement requires a valid clock")
        require(queue["completion_error_count"] == 0,
                location, "valid measurement cannot contain completion errors")
        require(queue["successful_poll_count"] >= 2,
                location, "valid measurement requires two successful polls")
        require(queue["timed_completion_count"] > 0,
                location, "valid measurement requires timed completions")
        require(queue["last_completion_tsc"] > queue["first_completion_tsc"],
                location, "valid measurement requires increasing timestamps")
        require(interval_cycles is not None and interval_cycles > 0,
                location, "valid interval cycles must be available and positive")
        require(interval_ns is not None and interval_ns > 0,
                location, "valid interval nanoseconds must be available and positive")
        require(rate_mpps is not None and rate_mpps > 0,
                location, "valid completion rate must be available and positive")
        expected_ns = interval_cycles / frequency
        expected_rate = frequency * 1000.0 / interval_cycles
        require(math.isclose(interval_ns, expected_ns, rel_tol=1e-12),
                location, "interval nanoseconds disagree with cycles/frequency")
        require(math.isclose(rate_mpps, expected_rate, rel_tol=1e-12),
                location, "completion rate disagrees with cycles/frequency")
    else:
        require(bool(reasons), location, "invalid measurement requires a reason")
        require(interval_cycles is None and interval_ns is None and rate_mpps is None,
                location, "unavailable queue metrics must be JSON null")


def validate_record(value: Any, location: str) -> tuple[str, int]:
    record = require_object(value, location)
    require_exact_keys(record, TOP_LEVEL_KEYS, location)
    require(record["schema"] == "axio.metrics/v1",
            f"{location}.schema", "unsupported schema")
    run_id = require_string(record["run_id"], f"{location}.run_id")
    window_id = require_uint(record["window_id"], f"{location}.window_id")

    identity = require_object(record["identity"], f"{location}.identity")
    require_exact_keys(identity, IDENTITY_KEYS, f"{location}.identity")
    role = require_string(identity["role"], f"{location}.identity.role")
    backend = require_string(identity["backend"], f"{location}.identity.backend")
    require(role in {"client", "server"},
            f"{location}.identity.role", "must be client or server")
    require(backend in {"dpdk", "roce"},
            f"{location}.identity.backend", "must be dpdk or roce")
    for key in IDENTITY_KEYS - {"role", "backend"}:
        require_string(identity[key], f"{location}.identity.{key}")

    window = require_object(record["window"], f"{location}.window")
    require_exact_keys(window, WINDOW_KEYS, f"{location}.window")
    require_number(window["duration_seconds"],
                   f"{location}.window.duration_seconds", positive=True)
    measurement_valid = require_bool(window["measurement_valid"],
                                     f"{location}.window.measurement_valid")
    require_bool(window["capacity_comparable"],
                 f"{location}.window.capacity_comparable")
    reasons = require_string_list(window["invalid_reasons"],
                                  f"{location}.window.invalid_reasons")
    if not measurement_valid:
        require(bool(reasons), location, "invalid window requires a reason")

    throughput = require_object(record["throughput"], f"{location}.throughput")
    require_exact_keys(throughput, {"e2e_mpps"}, f"{location}.throughput")
    require_metric(throughput["e2e_mpps"], f"{location}.throughput.e2e_mpps")

    latency = require_object(record["latency"], f"{location}.latency")
    require_exact_keys(latency, {"p50_us", "p99_us", "p999_us"},
                       f"{location}.latency")
    for key in ("p50_us", "p99_us", "p999_us"):
        require_metric(latency[key], f"{location}.latency.{key}")

    stages = require_object(record["stages"], f"{location}.stages")
    stage_keys = {"app_tx", "app_rx", "dispatcher_tx", "dispatcher_rx",
                  "nic_tx", "nic_rx"}
    require_exact_keys(stages, stage_keys, f"{location}.stages")
    for key in ("app_tx", "app_rx", "dispatcher_tx", "dispatcher_rx"):
        validate_stage(stages[key], f"{location}.stages.{key}")
    nic_tx = require_object(stages["nic_tx"], f"{location}.stages.nic_tx")
    require_exact_keys(nic_tx, {"throughput_mpps", "submit_time_per_packet_us"},
                       f"{location}.stages.nic_tx")
    require_metric(nic_tx["throughput_mpps"],
                   f"{location}.stages.nic_tx.throughput_mpps")
    require_metric(nic_tx["submit_time_per_packet_us"],
                   f"{location}.stages.nic_tx.submit_time_per_packet_us")
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
    if measurement_valid:
        require(all(value is not None and value > 0 for value in interval_values),
                location, "valid NIC RX aggregate metrics must be positive")
    else:
        require(all(value is None for value in interval_values),
                location, "unavailable NIC RX aggregate metrics must be JSON null")

    counters = require_object(record["counters"], f"{location}.counters")
    require_exact_keys(counters, COUNTER_KEYS, f"{location}.counters")
    for key in COUNTER_KEYS:
        require_uint(counters[key], f"{location}.counters.{key}")

    queues = record["queues"]
    require(isinstance(queues, list), f"{location}.queues", "must be an array")
    require(bool(queues), f"{location}.queues", "must contain dispatcher queues")
    workspace_ids: set[int] = set()
    for index, queue in enumerate(queues):
        queue_location = f"{location}.queues[{index}]"
        validate_queue(queue, queue_location)
        workspace_id = queue["workspace_id"]
        require(workspace_id not in workspace_ids,
                queue_location, "workspace_id must be unique within a window")
        workspace_ids.add(workspace_id)
    require(counters["nic_rx_successful_completion_count"] ==
            sum(queue["successful_completion_count"] for queue in queues),
            location, "NIC RX successful counter must equal the queue sum")
    require(counters["nic_rx_timed_completion_count"] ==
            sum(queue["timed_completion_count"] for queue in queues),
            location, "NIC RX timed counter must equal the queue sum")
    require(counters["nic_rx_completion_error_count"] ==
            sum(queue["completion_error_count"] for queue in queues),
            location, "NIC RX error counter must equal the queue sum")
    if measurement_valid:
        require(all(queue["measurement_valid"] for queue in queues),
                location, "valid aggregate requires every queue to be valid")
    else:
        require(any(not queue["measurement_valid"] for queue in queues),
                location, "invalid aggregate requires an invalid queue")
    return run_id, window_id


def reject_nonfinite(value: str) -> None:
    raise SchemaError(f"non-finite JSON constant {value}")


def parse_line(line: str, location: str) -> dict[str, Any]:
    try:
        value = json.loads(line, parse_constant=reject_nonfinite)
    except (json.JSONDecodeError, SchemaError) as error:
        raise SchemaError(f"{location}: invalid JSON: {error}") from error
    return require_object(value, location)


def validate_records(records: Iterable[tuple[dict[str, Any], str]]) -> int:
    seen: set[tuple[str, int]] = set()
    last_window: dict[str, int] = {}
    count = 0
    for record, location in records:
        run_id, window_id = validate_record(record, location)
        identity = (run_id, window_id)
        require(identity not in seen, location, "duplicate run/window ID")
        if run_id in last_window:
            require(window_id > last_window[run_id],
                    location, "window IDs must increase monotonically per run")
        seen.add(identity)
        last_window[run_id] = window_id
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
    mutation["schema"] = "axio.metrics/v2"
    rejects_record("unknown schema", mutation)
    mutation = copy.deepcopy(valid)
    del mutation["identity"]
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
    mutation["queues"][0]["measurement_valid"] = True
    rejects_record("invalid queue marked valid", mutation)
    expect_rejected(
        "duplicate run/window ID",
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
