"""Validation for optional Axio stage-distribution JSONL artifacts."""

from __future__ import annotations

import dataclasses
import json
import math
import pathlib
from typing import Any

from pipetune.model import ContractError


SCHEMA = "axio.stage-distribution/v1"
STAGE_NAMES = ("app_tx_allocation_stall", "app_rx_handler_completion")
METRIC_NAMES = ("p1_us", "p50_us", "p99_us", "mean_us", "min_us", "max_us")


def _unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ContractError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ContractError(f"non-finite JSON constant {value}")


def _object(value: object, location: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ContractError(f"{location} must be an object")
    return value


def _exact_keys(value: dict[str, Any], expected: set[str], location: str) -> None:
    if set(value) != expected:
        raise ContractError(f"{location} has an invalid field set")


def _number(value: object, location: str) -> float:
    if type(value) not in (int, float) or not math.isfinite(float(value)):
        raise ContractError(f"{location} must be a finite number")
    result = float(value)
    if result < 0.0:
        raise ContractError(f"{location} must not be negative")
    return result


@dataclasses.dataclass(frozen=True)
class DistributionSummary:
    p1_us: float
    p50_us: float
    p99_us: float
    mean_us: float
    min_us: float
    max_us: float
    sample_count: int


@dataclasses.dataclass(frozen=True)
class StageDistributionRecord:
    window_id: int
    sample_stride: int
    app_tx_allocation_stall: DistributionSummary
    app_rx_handler_completion: DistributionSummary


def _summary(value: object, location: str) -> DistributionSummary:
    document = _object(value, location)
    _exact_keys(document, {"unit", *METRIC_NAMES, "sample_count"}, location)
    if document["unit"] != "us_per_batch":
        raise ContractError(f"{location}.unit must be 'us_per_batch'")
    count = document["sample_count"]
    if type(count) is not int or count <= 0:
        raise ContractError(f"{location}.sample_count must be positive")
    metrics = {
        name: _number(document[name], f"{location}.{name}")
        for name in METRIC_NAMES
    }
    if not (
        metrics["min_us"] <= metrics["p1_us"] <= metrics["p50_us"]
        <= metrics["p99_us"] <= metrics["max_us"]
    ):
        raise ContractError(f"{location} percentiles are not ordered")
    return DistributionSummary(sample_count=count, **metrics)


def load_stage_distribution_jsonl(
    path: pathlib.Path,
) -> tuple[StageDistributionRecord, ...]:
    """Load a complete, newline-terminated stage-distribution stream."""

    try:
        payload = path.read_bytes()
    except OSError as error:
        raise ContractError(f"cannot read {path}: {error}") from error
    if not payload or not payload.endswith(b"\n"):
        raise ContractError(f"{path}: JSONL must be non-empty and newline-terminated")
    records: list[StageDistributionRecord] = []
    previous_window = -1
    for line_number, raw in enumerate(payload.splitlines(), start=1):
        try:
            value = json.loads(
                raw,
                parse_constant=_reject_constant,
                object_pairs_hook=_unique_object,
            )
        except (json.JSONDecodeError, ContractError) as error:
            raise ContractError(f"{path}:{line_number}: invalid JSON: {error}") from error
        document = _object(value, f"{path}:{line_number}")
        _exact_keys(
            document,
            {"schema", "window_id", "sample_stride", "stages"},
            f"{path}:{line_number}",
        )
        if document["schema"] != SCHEMA:
            raise ContractError(f"{path}:{line_number}: unsupported schema")
        window_id = document["window_id"]
        stride = document["sample_stride"]
        if type(window_id) is not int or window_id < 0 or window_id <= previous_window:
            raise ContractError(f"{path}:{line_number}: window IDs must increase")
        if type(stride) is not int or stride <= 0:
            raise ContractError(f"{path}:{line_number}: sample_stride must be positive")
        stages = _object(document["stages"], f"{path}:{line_number}.stages")
        _exact_keys(stages, set(STAGE_NAMES), f"{path}:{line_number}.stages")
        records.append(
            StageDistributionRecord(
                window_id=window_id,
                sample_stride=stride,
                app_tx_allocation_stall=_summary(
                    stages["app_tx_allocation_stall"],
                    f"{path}:{line_number}.stages.app_tx_allocation_stall",
                ),
                app_rx_handler_completion=_summary(
                    stages["app_rx_handler_completion"],
                    f"{path}:{line_number}.stages.app_rx_handler_completion",
                ),
            )
        )
        previous_window = window_id
    return tuple(records)
