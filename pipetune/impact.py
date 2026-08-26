"""Paper-aligned causal gate for PipeTune candidates."""

from __future__ import annotations

import dataclasses

from pipetune.diagnosis import (
    Diagnosis,
    Statistic,
    SteadySummary,
    statistic_from_samples,
)
from pipetune.search_policy import ImpactSpec


_DIRECTION_COUNTERS = {
    "tx": {"llc": "llc_store", "io": "io_read"},
    "rx": {"llc": "llc_load", "io": "io_write"},
}
_PIPELINE_STALL_COMPONENTS = (
    "app_rx.stall",
    "app_tx.stall",
    "dispatcher_rx.stall",
    "dispatcher_tx.stall",
)


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


def _impact_spec(value: Diagnosis | ImpactSpec) -> tuple[ImpactSpec, str]:
    if isinstance(value, Diagnosis):
        return ImpactSpec("diagnosis", value.point, value.direction), value.point
    point = value.metric if value.kind == "diagnosis" else value.kind
    return value, point or "unsupported_expected_impact"


def _counter_name(point: str, direction: str | None) -> str | None:
    names = _DIRECTION_COUNTERS.get(direction or "")
    if names is None:
        return None
    if point in ("P2", "P4"):
        return names["llc"]
    if point == "P3":
        return names["io"]
    return None


def _metric(
    impact: ImpactSpec,
    point: str,
    summary: SteadySummary,
    *,
    baseline_name: str | None,
) -> tuple[str, Statistic | None]:
    if impact.kind == "counter":
        name = impact.metric or "directional_counter"
        return name, summary.counters.get(name)

    if impact.kind == "component":
        name = impact.metric or "component_completion"
        if impact.metric is None:
            return name, None
        try:
            selected = summary.target.component(name)
        except KeyError:
            return name, None
        if selected.kind != "completion" or (
            impact.direction is not None and selected.direction != impact.direction
        ):
            return name, None
        return name, selected.statistic

    if impact.kind != "diagnosis":
        return "unsupported_expected_impact", None
    if point == "P1":
        component = summary.target.leading_component
        name = baseline_name or (
            component.name if component is not None else "leading_stall"
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

    name = _counter_name(point, impact.direction)
    if name is None:
        return "unsupported_expected_impact", None
    return name, summary.counters.get(name)


def _pipeline_stall(
    summary: SteadySummary,
    *,
    required_names: tuple[str, ...] | None,
) -> tuple[tuple[str, ...], Statistic | None]:
    selected: list[Statistic] = []
    names: list[str] = []
    candidates = required_names or _PIPELINE_STALL_COMPONENTS
    for name in candidates:
        try:
            component = summary.target.component(name)
        except KeyError:
            return tuple(names), None
        if component.kind != "stall":
            return tuple(names), None
        names.append(name)
        selected.append(component.statistic)
    if not selected or (required_names is not None and tuple(names) != required_names):
        return tuple(names), None
    unit = selected[0].unit
    if any(statistic.unit != unit for statistic in selected[1:]):
        return tuple(names), None
    sample_count = len(selected[0].samples)
    if sample_count == 0 or any(
        len(statistic.samples) != sample_count for statistic in selected[1:]
    ):
        return tuple(names), None
    samples = tuple(
        sum(statistic.samples[index] for statistic in selected)
        for index in range(sample_count)
    )
    return tuple(names), statistic_from_samples(
        samples,
        relative_floor=summary.noise_thresholds["stall_time_relative_floor"],
        unit=unit,
    )


def compare_expected_impact(
    impact: Diagnosis | ImpactSpec,
    baseline: SteadySummary,
    candidate: SteadySummary,
    *,
    candidate_id: str,
) -> ExpectedImpactComparison:
    """Require the paper's expected contention metric to significantly decrease."""

    spec, point = _impact_spec(impact)
    if spec.kind == "pipeline_stall":
        names, baseline_statistic = _pipeline_stall(
            baseline,
            required_names=None,
        )
        candidate_names, candidate_statistic = _pipeline_stall(
            candidate,
            required_names=names,
        )
        baseline_name = "pipeline_stall"
        candidate_name = (
            baseline_name if names == candidate_names else "unavailable_pipeline_stall"
        )
    else:
        baseline_name, baseline_statistic = _metric(
            spec,
            point,
            baseline,
            baseline_name=None,
        )
        candidate_name, candidate_statistic = _metric(
            spec,
            point,
            candidate,
            baseline_name=baseline_name,
        )
    if baseline_name != candidate_name:
        candidate_statistic = None
    if baseline_statistic is None or candidate_statistic is None:
        return ExpectedImpactComparison(
            candidate_id=candidate_id,
            point=point,
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
            point=point,
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
        point=point,
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
