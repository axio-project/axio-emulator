"""Paper-aligned causal gate for PipeTune candidates."""

from __future__ import annotations

import dataclasses

from pipetune.diagnosis import Diagnosis, Statistic, SteadySummary


_DIRECTION_COUNTERS = {
    "tx": {"llc": "llc_store", "io": "io_read"},
    "rx": {"llc": "llc_load", "io": "io_write"},
}


@dataclasses.dataclass(frozen=True)
class ExpectedImpactComparison:
    """Significance result for the contention metric a guideline should reduce."""

    candidate_id: str
    point: str
    metric: str
    accepted: bool
    reason: str
    baseline_value: float | None
    candidate_value: float | None
    observed_reduction: float | None
    required_reduction: float | None
    unit: str | None


def _counter_name(diagnosis: Diagnosis) -> str | None:
    names = _DIRECTION_COUNTERS.get(diagnosis.direction or "")
    if names is None:
        return None
    if diagnosis.point in ("P2", "P4"):
        return names["llc"]
    if diagnosis.point == "P3":
        return names["io"]
    return None


def _metric(
    diagnosis: Diagnosis,
    summary: SteadySummary,
    *,
    baseline_name: str | None,
) -> tuple[str, Statistic | None]:
    if diagnosis.point == "P1":
        component = summary.target.dominant_component
        name = baseline_name or (
            component.name if component is not None else "dominant_stall"
        )
        if component is None and baseline_name is None:
            return name, None
        try:
            selected = summary.target.component(name)
        except KeyError:
            return name, None
        if selected.kind != "stall":
            return name, None
        return name, selected.statistic

    name = _counter_name(diagnosis)
    if name is None:
        return "unsupported_expected_impact", None
    return name, summary.counters.get(name)


def compare_expected_impact(
    diagnosis: Diagnosis,
    baseline: SteadySummary,
    candidate: SteadySummary,
    *,
    candidate_id: str,
) -> ExpectedImpactComparison:
    """Require the paper's expected contention metric to significantly decrease."""

    baseline_name, baseline_statistic = _metric(
        diagnosis,
        baseline,
        baseline_name=None,
    )
    candidate_name, candidate_statistic = _metric(
        diagnosis,
        candidate,
        baseline_name=baseline_name,
    )
    if baseline_name != candidate_name:
        candidate_statistic = None
    if baseline_statistic is None or candidate_statistic is None:
        return ExpectedImpactComparison(
            candidate_id=candidate_id,
            point=diagnosis.point,
            metric=baseline_name,
            accepted=False,
            reason="expected-impact metric is unavailable",
            baseline_value=(
                baseline_statistic.median if baseline_statistic is not None else None
            ),
            candidate_value=(
                candidate_statistic.median if candidate_statistic is not None else None
            ),
            observed_reduction=None,
            required_reduction=None,
            unit=(
                baseline_statistic.unit if baseline_statistic is not None else None
            ),
        )
    if baseline_statistic.unit != candidate_statistic.unit:
        return ExpectedImpactComparison(
            candidate_id=candidate_id,
            point=diagnosis.point,
            metric=baseline_name,
            accepted=False,
            reason="expected-impact metric unit changed",
            baseline_value=baseline_statistic.median,
            candidate_value=candidate_statistic.median,
            observed_reduction=None,
            required_reduction=None,
            unit=baseline_statistic.unit,
        )

    observed = baseline_statistic.median - candidate_statistic.median
    required = max(
        baseline_statistic.uncertainty,
        candidate_statistic.uncertainty,
    )
    accepted = observed > required
    return ExpectedImpactComparison(
        candidate_id=candidate_id,
        point=diagnosis.point,
        metric=baseline_name,
        accepted=accepted,
        reason=(
            "expected-impact metric significantly decreases"
            if accepted
            else "expected-impact reduction is not significant"
        ),
        baseline_value=baseline_statistic.median,
        candidate_value=candidate_statistic.median,
        observed_reduction=observed,
        required_reduction=required,
        unit=baseline_statistic.unit,
    )


__all__ = ["ExpectedImpactComparison", "compare_expected_impact"]
