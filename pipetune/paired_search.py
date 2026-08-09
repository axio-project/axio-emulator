"""Pure threshold-guided search for paired colocated core counts."""

from __future__ import annotations

import dataclasses
import enum
import math
from typing import Any


MISS_RATE_THRESHOLD = 40.0


class PairedSearchError(ValueError):
    """Raised when persisted paired-search evidence is inconsistent."""


class PressureClass(str, enum.Enum):
    RELIEVED = "relieved"
    STRONG_BOUND = "strong_bound"
    MIXED = "mixed"
    UNAVAILABLE = "unavailable"


class SearchMode(str, enum.Enum):
    LINEAR = "linear"
    BINARY_SEEK = "binary_seek"
    BINARY_REFINE = "binary_refine"
    COMPUTE = "compute"
    FAILED = "failed"


def _optional_rate(value: object, name: str) -> float | None:
    if value is None:
        return None
    if type(value) not in (int, float):
        raise PairedSearchError(f"{name} must be a number or null")
    result = float(value)
    if not math.isfinite(result) or result < 0.0 or result > 100.0:
        raise PairedSearchError(f"{name} must be within 0..100")
    return result


@dataclasses.dataclass(frozen=True)
class PressureSample:
    count: int
    llc_rate: float | None
    io_rate: float | None
    llc_uncertainty: float | None
    io_uncertainty: float | None

    def __post_init__(self) -> None:
        if type(self.count) is not int or self.count < 1:
            raise PairedSearchError("pressure sample count must be positive")
        for name in (
            "llc_rate",
            "io_rate",
            "llc_uncertainty",
            "io_uncertainty",
        ):
            object.__setattr__(self, name, _optional_rate(getattr(self, name), name))
        if (self.llc_rate is None) != (self.llc_uncertainty is None):
            raise PairedSearchError("LLC rate and uncertainty must be available together")
        if (self.io_rate is None) != (self.io_uncertainty is None):
            raise PairedSearchError("I/O rate and uncertainty must be available together")

    def to_document(self) -> dict[str, object]:
        return dataclasses.asdict(self)

    @classmethod
    def from_document(cls, value: object) -> "PressureSample":
        if not isinstance(value, dict) or set(value) != {
            "count",
            "llc_rate",
            "io_rate",
            "llc_uncertainty",
            "io_uncertainty",
        }:
            raise PairedSearchError("pressure sample document is invalid")
        return cls(**value)


def classify_pressure(
    sample: PressureSample,
    *,
    threshold: float = MISS_RATE_THRESHOLD,
) -> PressureClass:
    """Classify both directional miss rates against one shared threshold."""

    if not math.isfinite(threshold) or threshold < 0.0 or threshold > 100.0:
        raise PairedSearchError("miss-rate threshold must be within 0..100")
    if sample.llc_rate is None or sample.io_rate is None:
        return PressureClass.UNAVAILABLE
    if sample.llc_rate <= threshold and sample.io_rate <= threshold:
        return PressureClass.RELIEVED
    if sample.llc_rate > threshold and sample.io_rate > threshold:
        return PressureClass.STRONG_BOUND
    return PressureClass.MIXED


def _significantly_reduces(reference: PressureSample, sample: PressureSample) -> bool:
    pairs = (
        (
            reference.llc_rate,
            reference.llc_uncertainty,
            sample.llc_rate,
            sample.llc_uncertainty,
        ),
        (
            reference.io_rate,
            reference.io_uncertainty,
            sample.io_rate,
            sample.io_uncertainty,
        ),
    )
    if any(value is None for pair in pairs for value in pair):
        return False
    return any(
        reference_rate - sample_rate
        > max(reference_uncertainty, sample_uncertainty)
        for (
            reference_rate,
            reference_uncertainty,
            sample_rate,
            sample_uncertainty,
        ) in pairs
    )


@dataclasses.dataclass(frozen=True)
class PairedSearchState:
    direction: str
    mode: SearchMode
    next_count: int | None
    high_pressure_count: int | None = None
    low_relief_count: int | None = None
    selected_count: int | None = None
    reference: PressureSample | None = None
    binary_validated: bool = False
    failure_reason: str | None = None
    threshold: float = MISS_RATE_THRESHOLD

    def __post_init__(self) -> None:
        if self.direction not in ("rx", "tx"):
            raise PairedSearchError("paired-search direction must be rx or tx")
        if not isinstance(self.mode, SearchMode):
            object.__setattr__(self, "mode", SearchMode(self.mode))
        for name in (
            "next_count",
            "high_pressure_count",
            "low_relief_count",
            "selected_count",
        ):
            value = getattr(self, name)
            if value is not None and (type(value) is not int or value < 1):
                raise PairedSearchError(f"{name} must be a positive integer or null")
        if self.mode in (SearchMode.COMPUTE, SearchMode.FAILED):
            if self.next_count is not None:
                raise PairedSearchError("terminal paired-search state has a next count")
        elif self.next_count is None:
            raise PairedSearchError("active paired-search state needs a next count")
        if self.mode is SearchMode.COMPUTE and self.selected_count is None:
            raise PairedSearchError("compute state needs a selected count")
        if self.mode is SearchMode.FAILED and not self.failure_reason:
            raise PairedSearchError("failed state needs a reason")

    @classmethod
    def start(cls, *, direction: str, count: int) -> "PairedSearchState":
        if type(count) is not int or count <= 1:
            raise PairedSearchError("paired search needs a count greater than one")
        return cls(
            direction=direction,
            mode=SearchMode.LINEAR,
            next_count=count - 1,
        )

    def _failed(self, reason: str) -> "PairedSearchState":
        return dataclasses.replace(
            self,
            mode=SearchMode.FAILED,
            next_count=None,
            failure_reason=reason,
        )

    def _compute(self, count: int) -> "PairedSearchState":
        return dataclasses.replace(
            self,
            mode=SearchMode.COMPUTE,
            next_count=None,
            selected_count=count,
        )

    def enter_compute(self, count: int) -> "PairedSearchState":
        """Stop count descent and compute-search from the last healthy cursor."""

        if self.mode in (SearchMode.COMPUTE, SearchMode.FAILED):
            raise PairedSearchError("terminal paired-search state cannot transition")
        if type(count) is not int or count < 1:
            raise PairedSearchError("compute cursor count must be positive")
        return self._compute(count)

    def observe(
        self,
        sample: PressureSample,
        *,
        objective_improved: bool,
    ) -> "PairedSearchState":
        """Consume the requested count and return the deterministic next state."""

        if self.mode in (SearchMode.COMPUTE, SearchMode.FAILED):
            raise PairedSearchError("terminal paired-search state cannot observe")
        if sample.count != self.next_count:
            raise PairedSearchError("pressure sample does not match the requested count")
        pressure = classify_pressure(sample, threshold=self.threshold)
        if pressure is PressureClass.UNAVAILABLE:
            return self._failed("directional_counters_unavailable")

        if self.mode is SearchMode.LINEAR:
            if pressure is PressureClass.RELIEVED:
                return dataclasses.replace(
                    self._compute(sample.count),
                    low_relief_count=sample.count,
                )
            if sample.count == 1:
                return self._failed("threshold_unreachable")
            if objective_improved or pressure is PressureClass.MIXED:
                return dataclasses.replace(self, next_count=sample.count - 1)
            return dataclasses.replace(
                self,
                mode=SearchMode.BINARY_SEEK,
                next_count=(1 + sample.count) // 2,
                high_pressure_count=sample.count,
                reference=sample,
            )

        validated = self.binary_validated
        if not validated:
            if self.reference is None or not _significantly_reduces(
                self.reference, sample
            ):
                return self._failed("contention_not_relieved")
            validated = True

        high = self.high_pressure_count
        low = self.low_relief_count
        if high is None:
            raise PairedSearchError("binary search has no pressure bound")
        if pressure is PressureClass.RELIEVED:
            low = sample.count
        else:
            high = sample.count
            if high == 1:
                return self._failed("threshold_unreachable")

        if low is not None and high - low <= 1:
            return dataclasses.replace(
                self._compute(low),
                high_pressure_count=high,
                low_relief_count=low,
                binary_validated=validated,
            )
        if low is None:
            next_count = (1 + high) // 2
            mode = SearchMode.BINARY_SEEK
        else:
            next_count = (low + high) // 2
            mode = SearchMode.BINARY_REFINE
        if next_count in (low, high):
            raise PairedSearchError("binary search did not make progress")
        return dataclasses.replace(
            self,
            mode=mode,
            next_count=next_count,
            high_pressure_count=high,
            low_relief_count=low,
            binary_validated=validated,
        )

    def to_document(self) -> dict[str, object]:
        return {
            "direction": self.direction,
            "mode": self.mode.value,
            "next_count": self.next_count,
            "high_pressure_count": self.high_pressure_count,
            "low_relief_count": self.low_relief_count,
            "selected_count": self.selected_count,
            "reference": self.reference.to_document() if self.reference else None,
            "binary_validated": self.binary_validated,
            "failure_reason": self.failure_reason,
            "threshold": self.threshold,
        }

    @classmethod
    def from_document(cls, value: object) -> "PairedSearchState":
        expected = {
            "direction",
            "mode",
            "next_count",
            "high_pressure_count",
            "low_relief_count",
            "selected_count",
            "reference",
            "binary_validated",
            "failure_reason",
            "threshold",
        }
        if not isinstance(value, dict) or set(value) != expected:
            raise PairedSearchError("paired-search state document is invalid")
        document: dict[str, Any] = dict(value)
        reference = document["reference"]
        document["reference"] = (
            PressureSample.from_document(reference) if reference is not None else None
        )
        return cls(**document)


__all__ = [
    "MISS_RATE_THRESHOLD",
    "PairedSearchError",
    "PairedSearchState",
    "PressureClass",
    "PressureSample",
    "SearchMode",
    "classify_pressure",
]
