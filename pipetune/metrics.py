"""Strict reader for Axio's compact six-stage JSONL window contract."""

from __future__ import annotations

import dataclasses
import json
import math
import pathlib
from typing import Any, Iterable

from pipetune.model import ContractError


AXIO_METRICS_SCHEMA = "axio.metrics/v1"
STAGE_NAMES = (
    "app_tx",
    "app_rx",
    "dispatcher_tx",
    "dispatcher_rx",
    "nic_tx",
    "nic_rx",
)


def _require(condition: bool, location: str, message: str) -> None:
    if not condition:
        raise ContractError(f"{location}: {message}")


def _object(value: object, location: str) -> dict[str, Any]:
    _require(isinstance(value, dict), location, "must be an object")
    return value


def _exact_keys(value: dict[str, Any], expected: set[str], location: str) -> None:
    missing = sorted(expected - value.keys())
    unknown = sorted(value.keys() - expected)
    _require(not missing, location, f"missing required keys {missing}")
    _require(not unknown, location, f"unknown keys {unknown}")


def _uint(value: object, location: str) -> int:
    _require(type(value) is int and value >= 0, location, "must be a non-negative integer")
    return value


def _metric(value: object, location: str) -> float | None:
    if value is None:
        return None
    _require(type(value) in (int, float), location, "must be a number or null")
    number = float(value)
    _require(math.isfinite(number), location, "must be finite")
    _require(number >= 0.0, location, "must be non-negative")
    return number


@dataclasses.dataclass(frozen=True)
class PipelineStage:
    completion_time_per_packet_us: float | None
    stall_time_per_packet_us: float | None


@dataclasses.dataclass(frozen=True)
class NicTxStage:
    throughput_mpps: float | None
    submit_time_per_packet_us: float | None


@dataclasses.dataclass(frozen=True)
class NicRxStage:
    throughput_mpps: float | None
    completion_interval_cycles: float | None
    completion_interval_ns: float | None
    slowest_interval_cycles: float | None
    capacity_interval_cycles: float | None


@dataclasses.dataclass(frozen=True)
class WindowCounters:
    app_enqueue_drop_count: int
    dispatcher_enqueue_drop_count: int
    nic_rx_completion_error_count: int


@dataclasses.dataclass(frozen=True)
class AxioWindow:
    window_id: int
    e2e_throughput_mpps: float | None
    latency_p50_us: float | None
    latency_p99_us: float | None
    latency_p999_us: float | None
    app_tx: PipelineStage
    app_rx: PipelineStage
    dispatcher_tx: PipelineStage
    dispatcher_rx: PipelineStage
    nic_tx: NicTxStage
    nic_rx: NicRxStage
    counters: WindowCounters

    @property
    def stage_names(self) -> tuple[str, ...]:
        return STAGE_NAMES


def _pipeline_stage(value: object, location: str) -> PipelineStage:
    stage = _object(value, location)
    _exact_keys(
        stage,
        {"completion_time_per_packet_us", "stall_time_per_packet_us"},
        location,
    )
    return PipelineStage(
        completion_time_per_packet_us=_metric(
            stage["completion_time_per_packet_us"],
            f"{location}.completion_time_per_packet_us",
        ),
        stall_time_per_packet_us=_metric(
            stage["stall_time_per_packet_us"],
            f"{location}.stall_time_per_packet_us",
        ),
    )


def parse_axio_window(value: object, location: str) -> AxioWindow:
    record = _object(value, location)
    _exact_keys(
        record,
        {"window_id", "throughput", "latency", "stages", "counters"},
        location,
    )
    throughput = _object(record["throughput"], f"{location}.throughput")
    _exact_keys(throughput, {"e2e_mpps"}, f"{location}.throughput")
    latency = _object(record["latency"], f"{location}.latency")
    _exact_keys(latency, {"p50_us", "p99_us", "p999_us"}, f"{location}.latency")
    stages = _object(record["stages"], f"{location}.stages")
    _exact_keys(stages, set(STAGE_NAMES), f"{location}.stages")

    nic_tx = _object(stages["nic_tx"], f"{location}.stages.nic_tx")
    _exact_keys(
        nic_tx,
        {"throughput_mpps", "submit_time_per_packet_us"},
        f"{location}.stages.nic_tx",
    )
    nic_rx = _object(stages["nic_rx"], f"{location}.stages.nic_rx")
    nic_rx_keys = {
        "throughput_mpps",
        "completion_interval_cycles",
        "completion_interval_ns",
        "slowest_interval_cycles",
        "capacity_interval_cycles",
    }
    _exact_keys(nic_rx, nic_rx_keys, f"{location}.stages.nic_rx")
    intervals = tuple(
        _metric(nic_rx[key], f"{location}.stages.nic_rx.{key}")
        for key in (
            "completion_interval_cycles",
            "completion_interval_ns",
            "slowest_interval_cycles",
            "capacity_interval_cycles",
        )
    )
    _require(
        all(interval is None for interval in intervals)
        or all(interval is not None and interval > 0.0 for interval in intervals),
        f"{location}.stages.nic_rx",
        "intervals must be all positive or all null",
    )

    counters = _object(record["counters"], f"{location}.counters")
    counter_keys = {
        "app_enqueue_drop_count",
        "dispatcher_enqueue_drop_count",
        "nic_rx_completion_error_count",
    }
    _exact_keys(counters, counter_keys, f"{location}.counters")
    return AxioWindow(
        window_id=_uint(record["window_id"], f"{location}.window_id"),
        e2e_throughput_mpps=_metric(
            throughput["e2e_mpps"], f"{location}.throughput.e2e_mpps"
        ),
        latency_p50_us=_metric(latency["p50_us"], f"{location}.latency.p50_us"),
        latency_p99_us=_metric(latency["p99_us"], f"{location}.latency.p99_us"),
        latency_p999_us=_metric(
            latency["p999_us"], f"{location}.latency.p999_us"
        ),
        app_tx=_pipeline_stage(stages["app_tx"], f"{location}.stages.app_tx"),
        app_rx=_pipeline_stage(stages["app_rx"], f"{location}.stages.app_rx"),
        dispatcher_tx=_pipeline_stage(
            stages["dispatcher_tx"], f"{location}.stages.dispatcher_tx"
        ),
        dispatcher_rx=_pipeline_stage(
            stages["dispatcher_rx"], f"{location}.stages.dispatcher_rx"
        ),
        nic_tx=NicTxStage(
            throughput_mpps=_metric(
                nic_tx["throughput_mpps"],
                f"{location}.stages.nic_tx.throughput_mpps",
            ),
            submit_time_per_packet_us=_metric(
                nic_tx["submit_time_per_packet_us"],
                f"{location}.stages.nic_tx.submit_time_per_packet_us",
            ),
        ),
        nic_rx=NicRxStage(
            throughput_mpps=_metric(
                nic_rx["throughput_mpps"],
                f"{location}.stages.nic_rx.throughput_mpps",
            ),
            completion_interval_cycles=intervals[0],
            completion_interval_ns=intervals[1],
            slowest_interval_cycles=intervals[2],
            capacity_interval_cycles=intervals[3],
        ),
        counters=WindowCounters(
            app_enqueue_drop_count=_uint(
                counters["app_enqueue_drop_count"],
                f"{location}.counters.app_enqueue_drop_count",
            ),
            dispatcher_enqueue_drop_count=_uint(
                counters["dispatcher_enqueue_drop_count"],
                f"{location}.counters.dispatcher_enqueue_drop_count",
            ),
            nic_rx_completion_error_count=_uint(
                counters["nic_rx_completion_error_count"],
                f"{location}.counters.nic_rx_completion_error_count",
            ),
        ),
    )


def _reject_constant(value: str) -> None:
    raise ContractError(f"non-finite JSON constant {value}")


def _unique_object(pairs: Iterable[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ContractError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def load_axio_jsonl(
    path: pathlib.Path, *, schema: str
) -> tuple[AxioWindow, ...]:
    _require(schema == AXIO_METRICS_SCHEMA, str(path), "unsupported metrics schema")
    windows: list[AxioWindow] = []
    last_window_id: int | None = None
    with path.open(encoding="utf-8") as source:
        for line_number, raw_line in enumerate(source, start=1):
            location = f"{path}:{line_number}"
            line = raw_line.rstrip("\n")
            _require(bool(line), location, "blank JSONL lines are forbidden")
            try:
                document = json.loads(
                    line,
                    parse_constant=_reject_constant,
                    object_pairs_hook=_unique_object,
                )
            except (json.JSONDecodeError, ContractError) as error:
                raise ContractError(f"{location}: invalid JSON: {error}") from error
            window = parse_axio_window(document, location)
            if last_window_id is not None:
                _require(
                    window.window_id > last_window_id,
                    location,
                    "window IDs must increase uniquely and monotonically",
                )
            windows.append(window)
            last_window_id = window.window_id
    _require(bool(windows), str(path), "metrics JSONL must not be empty")
    return tuple(windows)
