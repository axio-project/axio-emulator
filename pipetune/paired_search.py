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
    UNAVAILABLE = "unavailable"


class SearchMode(str, enum.Enum):
    LINEAR = "linear"
    BINARY_SEEK = "binary_seek"
    LOCAL = "local"
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
    """Use directional LLC miss for the search boundary; I/O is evidence only."""

    if not math.isfinite(threshold) or threshold < 0.0 or threshold > 100.0:
        raise PairedSearchError("miss-rate threshold must be within 0..100")
    if sample.llc_rate is None:
        return PressureClass.UNAVAILABLE
    if sample.llc_rate <= threshold:
        return PressureClass.RELIEVED
    return PressureClass.STRONG_BOUND


def _significantly_reduces(reference: PressureSample, sample: PressureSample) -> bool:
    pairs = (
        (
            reference.llc_rate,
            reference.llc_uncertainty,
            sample.llc_rate,
            sample.llc_uncertainty,
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
            try:
                mode = SearchMode(self.mode)
            except (TypeError, ValueError) as error:
                raise PairedSearchError(
                    "paired-search mode is invalid"
                ) from error
            object.__setattr__(self, "mode", mode)
        for name in (
            "next_count",
            "high_pressure_count",
            "low_relief_count",
            "selected_count",
        ):
            value = getattr(self, name)
            if value is not None and (type(value) is not int or value < 1):
                raise PairedSearchError(f"{name} must be a positive integer or null")
        threshold = _optional_rate(self.threshold, "threshold")
        if threshold is None:
            raise PairedSearchError("threshold must be within 0..100")
        object.__setattr__(self, "threshold", threshold)
        if type(self.binary_validated) is not bool:
            raise PairedSearchError("binary_validated must be boolean")
        if self.reference is not None and not isinstance(
            self.reference, PressureSample
        ):
            raise PairedSearchError("reference must be a pressure sample or null")
        if self.failure_reason is not None and (
            not isinstance(self.failure_reason, str) or not self.failure_reason
        ):
            raise PairedSearchError("failure_reason must be a non-empty string or null")
        if (
            self.low_relief_count is not None
            and self.high_pressure_count is not None
            and self.low_relief_count >= self.high_pressure_count
        ):
            raise PairedSearchError(
                "binary bounds require low_relief_count < high_pressure_count"
            )
        if self.mode in (SearchMode.COMPUTE, SearchMode.FAILED):
            if self.next_count is not None:
                raise PairedSearchError("terminal paired-search state has a next count")
        elif self.next_count is None:
            raise PairedSearchError("active paired-search state needs a next count")
        if self.mode in (SearchMode.LOCAL, SearchMode.COMPUTE) and self.selected_count is None:
            raise PairedSearchError("local/compute state needs a selected count")
        if self.mode is SearchMode.FAILED and not self.failure_reason:
            raise PairedSearchError("failed state needs a reason")
        if self.mode not in (SearchMode.LOCAL, SearchMode.COMPUTE) and self.selected_count is not None:
            raise PairedSearchError("only local/compute state may select a count")
        if self.mode is not SearchMode.FAILED and self.failure_reason is not None:
            raise PairedSearchError("only failed state may have a failure reason")

        if self.mode is SearchMode.LINEAR:
            if any(
                value is not None
                for value in (
                    self.high_pressure_count,
                    self.low_relief_count,
                    self.reference,
                )
            ) or self.binary_validated:
                raise PairedSearchError("linear state cannot contain binary evidence")
        elif self.mode is SearchMode.BINARY_SEEK:
            if (
                self.high_pressure_count is None
                or self.low_relief_count is not None
                or self.reference is None
                or self.next_count is None
                or self.next_count >= self.high_pressure_count
            ):
                raise PairedSearchError("binary seek state has inconsistent bounds")
        elif self.mode is SearchMode.LOCAL:
            if (
                self.low_relief_count is None
                or self.selected_count > self.low_relief_count
                or self.next_count not in (self.selected_count, self.selected_count - 1)
                or self.high_pressure_count is not None
                or self.reference is not None
            ):
                raise PairedSearchError("local search has inconsistent count cursor")

        if self.reference is not None:
            if (
                classify_pressure(self.reference, threshold=self.threshold)
                is not PressureClass.STRONG_BOUND
            ):
                raise PairedSearchError("binary reference must be strongly memory-bound")
            if (
                self.high_pressure_count is not None
                and self.reference.count < self.high_pressure_count
            ):
                raise PairedSearchError("binary reference precedes its pressure bound")
            if (
                not self.binary_validated
                and self.high_pressure_count is not None
                and self.reference.count != self.high_pressure_count
            ):
                raise PairedSearchError(
                    "unvalidated binary reference must be the high bound"
                )
        elif self.binary_validated:
            raise PairedSearchError("validated binary state needs a reference")

        if self.mode is SearchMode.COMPUTE:
            if (
                self.low_relief_count is not None
                and self.selected_count != self.low_relief_count
            ):
                raise PairedSearchError("compute selection must use the relief bound")
            if (
                self.low_relief_count is None
                and self.high_pressure_count is not None
                and self.selected_count != self.high_pressure_count
            ):
                raise PairedSearchError("compute selection must use the pressure cursor")

    @classmethod
    def start(cls, *, direction: str, count: int) -> "PairedSearchState":
        if type(count) is not int or count <= 1:
            raise PairedSearchError("paired search needs a count greater than one")
        return cls(
            direction=direction,
            mode=SearchMode.LINEAR,
            next_count=count - 1,
        )

    @classmethod
    def compute_from_cursor(
        cls, *, direction: str, count: int
    ) -> "PairedSearchState":
        """Start compute search from a previously verified healthy cursor."""

        if type(count) is not int or count < 1:
            raise PairedSearchError("compute cursor count must be positive")
        return cls(
            direction=direction,
            mode=SearchMode.COMPUTE,
            next_count=None,
            selected_count=count,
        )

    def _failed(self, reason: str) -> "PairedSearchState":
        return dataclasses.replace(
            self,
            mode=SearchMode.FAILED,
            next_count=None,
            failure_reason=reason,
        )

    def _compute(self, count: int) -> "PairedSearchState":
        return self.compute_from_cursor(direction=self.direction, count=count)

    def local_from_cursor(self, count: int) -> "PairedSearchState":
        """Compare placements at the first relieved count before descending."""

        return PairedSearchState(
            direction=self.direction,
            mode=SearchMode.LOCAL,
            next_count=count,
            selected_count=count,
            low_relief_count=count,
            threshold=self.threshold,
        )

    def advance_local(self, *, accepted: bool) -> "PairedSearchState":
        """Both placements were evaluated; retain the prior count on regression."""

        if self.mode is not SearchMode.LOCAL:
            raise PairedSearchError("placement comparison requires local search")
        count = self.next_count
        first_placement = count == self.selected_count
        if not accepted and not first_placement:
            return self._compute(self.selected_count)
        if count == 1:
            return self._compute(count)
        return dataclasses.replace(self, next_count=count - 1, selected_count=count)

    def enter_compute(self, count: int) -> "PairedSearchState":
        """Stop count descent and compute-search from the last healthy cursor."""

        if self.mode not in (SearchMode.LINEAR, SearchMode.BINARY_SEEK):
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

        if self.mode not in (SearchMode.LINEAR, SearchMode.BINARY_SEEK):
            raise PairedSearchError("terminal paired-search state cannot observe")
        if sample.count != self.next_count:
            raise PairedSearchError("pressure sample does not match the requested count")
        pressure = classify_pressure(sample, threshold=self.threshold)
        if pressure is PressureClass.UNAVAILABLE:
            return self._failed("directional_llc_unavailable")
        if pressure is PressureClass.RELIEVED:
            return self.local_from_cursor(sample.count)

        if self.mode is SearchMode.LINEAR:
            if sample.count == 1:
                return self._failed("threshold_unreachable")
            if objective_improved:
                return dataclasses.replace(self, next_count=sample.count - 1)
            return dataclasses.replace(
                self,
                mode=SearchMode.BINARY_SEEK,
                next_count=(1 + sample.count) // 2,
                high_pressure_count=sample.count,
                reference=sample,
            )

        validated = self.binary_validated
        high = self.high_pressure_count
        if high is None:
            raise PairedSearchError("binary search has no pressure bound")
        if not validated:
            if self.reference is None or not _significantly_reduces(self.reference, sample):
                return self._failed("contention_not_relieved")
            validated = True
        high = sample.count
        if high == 1:
            return self._failed("threshold_unreachable")
        return dataclasses.replace(
            self,
            next_count=(1 + high) // 2,
            high_pressure_count=high,
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
