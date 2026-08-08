"""Bounded perf-stat command generation and strict LLC-rate parsing."""

from __future__ import annotations

import csv
import io
import math

from pipetune.model import CounterValue, ContractError, ProviderStatus
from pipetune.providers import (
    ProviderResult,
    available_counter,
    unavailable_counter,
)


PERF_EVENTS = (
    "LLC-loads",
    "LLC-load-misses",
    "LLC-stores",
    "LLC-store-misses",
)

PERF_EVENT_GROUPS = (
    ("LLC-loads", "LLC-load-misses"),
    ("LLC-stores", "LLC-store-misses"),
)


class PerfProvider:
    def __init__(self, path: str, version: str) -> None:
        if not path or not version:
            raise ContractError("perf path and version must not be empty")
        self.path = path
        self.version = version

    @staticmethod
    def version_command(path: str) -> tuple[str, ...]:
        if not path:
            raise ContractError("perf path must not be empty")
        return ("env", "LC_ALL=C", path, "--version")

    @staticmethod
    def parse_version(stdout: bytes) -> str:
        try:
            version = stdout.decode("utf-8", errors="strict").strip()
        except UnicodeDecodeError as error:
            raise ValueError("perf version is not valid UTF-8") from error
        if not version.startswith("perf version ") or "\n" in version:
            raise ValueError("perf returned an invalid version")
        return version

    def commands(
        self,
        *,
        pid: int,
        sample_interval_seconds: float,
        period_milliseconds: int = 1000,
    ) -> tuple[tuple[str, ...], ...]:
        if type(pid) is not int or pid <= 0:
            raise ContractError("perf target PID must be positive")
        if (
            type(sample_interval_seconds) not in (int, float)
            or not math.isfinite(sample_interval_seconds)
            or sample_interval_seconds <= 0
        ):
            raise ContractError("perf sample interval must be positive and finite")
        if type(period_milliseconds) is not int or period_milliseconds <= 0:
            raise ContractError("perf period must be a positive integer")
        return tuple(
            (
                "env",
                "LC_ALL=C",
                self.path,
                "stat",
                "--no-big-num",
                "-x",
                ";",
                "-I",
                str(period_milliseconds),
                "-e",
                ",".join(events),
                "-p",
                str(pid),
                "--",
                "sleep",
                format(float(sample_interval_seconds), "g"),
            )
            for events in PERF_EVENT_GROUPS
        )

    def _status(self, *, available: bool, reason: str | None) -> ProviderStatus:
        return ProviderStatus(
            name="perf",
            available=available,
            path=self.path,
            version=self.version if available else None,
            reason=reason,
        )

    def parse(
        self,
        raw_stderr: bytes,
        *,
        sample_interval_seconds: float,
        minimum_running_percent: float = 90.0,
    ) -> ProviderResult:
        if (
            type(sample_interval_seconds) not in (int, float)
            or not math.isfinite(sample_interval_seconds)
            or sample_interval_seconds <= 0
        ):
            raise ContractError("perf sample interval must be positive and finite")
        if (
            type(minimum_running_percent) not in (int, float)
            or not math.isfinite(minimum_running_percent)
            or not 0 <= minimum_running_percent <= 100
        ):
            raise ContractError("perf minimum running percent must be in [0, 100]")
        unavailable_names = ("llc_load", "llc_store")
        try:
            text = raw_stderr.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            reason = "perf output is not valid UTF-8"
            return ProviderResult(
                self._status(available=False, reason=reason),
                tuple(unavailable_counter(name, reason) for name in unavailable_names),
            )
        lowered = text.casefold()
        if "permission" in lowered or "access denied" in lowered:
            reason = "perf permission failure"
            return ProviderResult(
                self._status(available=False, reason=reason),
                tuple(unavailable_counter(name, reason) for name in unavailable_names),
            )
        records: dict[str, list[tuple[float, float, float]]] = {
            event: [] for event in PERF_EVENTS
        }
        errors: dict[str, str] = {}
        for row in csv.reader(io.StringIO(text), delimiter=";"):
            if not row or row[0].lstrip().startswith("#"):
                continue
            event_indexes = [
                index for index, field in enumerate(row) if field.strip() in PERF_EVENTS
            ]
            if not event_indexes:
                continue
            if len(event_indexes) != 1:
                continue
            event_index = event_indexes[0]
            event = row[event_index].strip()
            if event_index < 3 or len(row) <= event_index + 2:
                errors[event] = "perf periodic sample is truncated"
                continue
            count_text = next(
                (field.strip() for field in reversed(row[1:event_index]) if field.strip()),
                "",
            )
            if count_text.startswith("<"):
                errors[event] = "perf event is unsupported or not counted"
                continue
            try:
                timestamp = _number(row[0], "timestamp")
                count = _number(count_text, "counter")
                running = _number(row[event_index + 2], "running percent")
            except ValueError:
                errors[event] = "perf sample has invalid numeric fields"
                continue
            records[event].append((timestamp, count, running))

        load = self._counter(
            "llc_load",
            records,
            errors,
            reference_event="LLC-loads",
            miss_event="LLC-load-misses",
            sample_interval_seconds=sample_interval_seconds,
            minimum_running_percent=minimum_running_percent,
        )
        store = self._counter(
            "llc_store",
            records,
            errors,
            reference_event="LLC-stores",
            miss_event="LLC-store-misses",
            sample_interval_seconds=sample_interval_seconds,
            minimum_running_percent=minimum_running_percent,
        )
        return ProviderResult(self._status(available=True, reason=None), (load, store))

    def _counter(
        self,
        name: str,
        records: dict[str, list[tuple[float, float, float]]],
        errors: dict[str, str],
        *,
        reference_event: str,
        miss_event: str,
        sample_interval_seconds: float,
        minimum_running_percent: float,
    ) -> CounterValue:
        for event in (reference_event, miss_event):
            if event in errors:
                return unavailable_counter(name, errors[event])
        references = records[reference_event]
        misses = records[miss_event]
        if not references or len(references) != len(misses):
            return unavailable_counter(name, "perf periodic sample is truncated")
        if [row[0] for row in references] != [row[0] for row in misses]:
            return unavailable_counter(name, "perf periodic timestamps are inconsistent")
        if references[-1][0] < sample_interval_seconds * 0.9:
            return unavailable_counter(name, "perf sample has insufficient running time")
        if any(
            row[2] < minimum_running_percent for row in (*references, *misses)
        ):
            return unavailable_counter(name, "perf event has insufficient running time")
        denominator = sum(row[1] for row in references)
        numerator = sum(row[1] for row in misses)
        if denominator <= 0:
            return unavailable_counter(name, "perf counter has zero denominator")
        try:
            return available_counter(name, numerator, denominator)
        except ContractError:
            return unavailable_counter(name, "perf counter values are inconsistent")


def _number(value: str, field: str) -> float:
    stripped = value.strip()
    if not stripped or "," in stripped:
        raise ValueError(f"{field} is not locale-stable")
    number = float(stripped)
    if not math.isfinite(number) or number < 0:
        raise ValueError(f"{field} is invalid")
    return number
