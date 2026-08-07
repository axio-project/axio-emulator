"""Typed host-metric provider contracts."""

from __future__ import annotations

import dataclasses

from pipetune.model import CounterValue, ProviderStatus


@dataclasses.dataclass(frozen=True)
class ProviderResult:
    status: ProviderStatus
    counters: tuple[CounterValue, ...]


def unavailable_counter(name: str, reason: str) -> CounterValue:
    return CounterValue(
        name=name,
        available=False,
        numerator=None,
        denominator=None,
        rate_percent=None,
        reason=reason,
    )


def available_counter(name: str, numerator: float, denominator: float) -> CounterValue:
    return CounterValue(
        name=name,
        available=True,
        numerator=numerator,
        denominator=denominator,
        rate_percent=numerator / denominator * 100.0,
        reason=None,
    )


__all__ = ["ProviderResult"]
