"""Latency-feasible throughput objective for PipeTune cold-start trials."""

from __future__ import annotations

import dataclasses
import math
from collections.abc import Iterable

from pipetune.diagnosis import Statistic, SteadySummary


class ObjectiveError(ValueError):
    """Raised when objective inputs violate the tuning state contract."""


TRIAL_STATUSES = (
    "valid",
    "invalid",
    "unhealthy_peer",
    "infrastructure_failure",
)


def _valid_statistic(value: Statistic, unit: str) -> bool:
    return (
        value.unit == unit
        and math.isfinite(value.median)
        and value.median >= 0.0
        and math.isfinite(value.mad)
        and value.mad >= 0.0
    )


@dataclasses.dataclass(frozen=True)
class ObjectiveTrial:
    """The only trial fields allowed to participate in objective selection."""

    trial_id: str
    status: str
    client_p999: Statistic | None
    server_throughput: Statistic | None
    rejection_reason: str | None

    def __post_init__(self) -> None:
        if not self.trial_id:
            raise ObjectiveError("objective trial ID must not be empty")
        if self.status not in TRIAL_STATUSES:
            raise ObjectiveError(f"unsupported objective trial status {self.status!r}")
        if self.status == "valid":
            if self.client_p999 is None or not _valid_statistic(
                self.client_p999, "us"
            ):
                raise ObjectiveError("valid trial requires client P99.9 in us")
            if self.server_throughput is None or not _valid_statistic(
                self.server_throughput, "Mpps"
            ):
                raise ObjectiveError("valid trial requires server throughput in Mpps")
            if self.rejection_reason is not None:
                raise ObjectiveError("valid trial must not have a rejection reason")
        elif (
            self.client_p999 is not None
            or self.server_throughput is not None
            or not self.rejection_reason
        ):
            raise ObjectiveError(
                "rejected trial must contain only a non-empty rejection reason"
            )


@dataclasses.dataclass(frozen=True)
class ObjectivePolicy:
    latency_slo_us: float
    latency_relative_floor: float
    throughput_relative_floor: float

    def __post_init__(self) -> None:
        if not math.isfinite(self.latency_slo_us) or self.latency_slo_us <= 0.0:
            raise ObjectiveError("latency SLO must be positive and finite")
        for name, value in (
            ("latency_relative_floor", self.latency_relative_floor),
            ("throughput_relative_floor", self.throughput_relative_floor),
        ):
            if not math.isfinite(value) or value < 0.0:
                raise ObjectiveError(f"{name} must be non-negative and finite")


@dataclasses.dataclass(frozen=True)
class ObjectiveComparison:
    candidate_id: str
    accepted: bool
    reason: str
    metric: str | None
    observed_improvement: float | None
    required_improvement: float | None
    accepted_feasible: bool
    candidate_feasible: bool | None


def _rejected_trial(trial_id: str, status: str, reason: str) -> ObjectiveTrial:
    return ObjectiveTrial(
        trial_id=trial_id,
        status=status,
        client_p999=None,
        server_throughput=None,
        rejection_reason=reason,
    )


def objective_trial_from_summary(summary: SteadySummary) -> ObjectiveTrial:
    """Extract role-stable objective metrics from one steady-state summary."""

    if not summary.peer_health.healthy:
        reason = "; ".join(summary.peer_health.reasons) or "peer health check failed"
        return _rejected_trial(summary.trial_id, "unhealthy_peer", reason)

    endpoints = (summary.target, summary.peer)
    by_role = {endpoint.spec.role: endpoint for endpoint in endpoints}
    if set(by_role) != {"client", "server"} or len(by_role) != len(endpoints):
        return _rejected_trial(
            summary.trial_id,
            "invalid",
            "objective requires exactly one client and one server endpoint",
        )
    client_p999 = by_role["client"].latency.get("p999_us")
    if client_p999 is None:
        return _rejected_trial(
            summary.trial_id,
            "invalid",
            "client P99.9 is unavailable",
        )
    try:
        return ObjectiveTrial(
            trial_id=summary.trial_id,
            status="valid",
            client_p999=client_p999,
            server_throughput=by_role["server"].throughput,
            rejection_reason=None,
        )
    except ObjectiveError as error:
        return _rejected_trial(summary.trial_id, "invalid", str(error))


def _is_feasible(trial: ObjectiveTrial, policy: ObjectivePolicy) -> bool:
    if trial.status != "valid" or trial.client_p999 is None:
        raise ObjectiveError("latency feasibility requires a valid objective trial")
    return trial.client_p999.median <= policy.latency_slo_us


def _metrics(trial: ObjectiveTrial) -> tuple[Statistic, Statistic]:
    if (
        trial.status != "valid"
        or trial.client_p999 is None
        or trial.server_throughput is None
    ):
        raise ObjectiveError("objective metrics require a valid trial")
    return trial.client_p999, trial.server_throughput


def _required_improvement(
    accepted: Statistic,
    candidate: Statistic,
    relative_floor: float,
) -> float:
    return max(
        abs(accepted.median) * relative_floor,
        3.0 * accepted.mad,
        abs(candidate.median) * relative_floor,
        3.0 * candidate.mad,
    )


def compare_candidate(
    accepted: ObjectiveTrial,
    candidate: ObjectiveTrial,
    policy: ObjectivePolicy,
) -> ObjectiveComparison:
    """Decide whether one candidate significantly improves accepted state."""

    if accepted.status != "valid":
        raise ObjectiveError("accepted objective state must be valid")
    accepted_feasible = _is_feasible(accepted, policy)
    if candidate.status != "valid":
        return ObjectiveComparison(
            candidate_id=candidate.trial_id,
            accepted=False,
            reason=f"{candidate.status}: {candidate.rejection_reason}",
            metric=None,
            observed_improvement=None,
            required_improvement=None,
            accepted_feasible=accepted_feasible,
            candidate_feasible=None,
        )

    candidate_feasible = _is_feasible(candidate, policy)
    accepted_latency, accepted_throughput = _metrics(accepted)
    candidate_latency, candidate_throughput = _metrics(candidate)
    if not accepted_feasible:
        improvement = accepted_latency.median - candidate_latency.median
        required = _required_improvement(
            accepted_latency,
            candidate_latency,
            policy.latency_relative_floor,
        )
        significant = improvement > required
        if not significant:
            reason = "latency improvement is not significant"
        elif candidate_feasible:
            reason = "candidate reaches latency feasibility"
        else:
            reason = "candidate significantly reduces latency"
        return ObjectiveComparison(
            candidate_id=candidate.trial_id,
            accepted=significant,
            reason=reason,
            metric="client_p999_us",
            observed_improvement=improvement,
            required_improvement=required,
            accepted_feasible=False,
            candidate_feasible=candidate_feasible,
        )

    if not candidate_feasible:
        return ObjectiveComparison(
            candidate_id=candidate.trial_id,
            accepted=False,
            reason="candidate violates latency SLO",
            metric="client_p999_us",
            observed_improvement=None,
            required_improvement=None,
            accepted_feasible=True,
            candidate_feasible=False,
        )

    improvement = candidate_throughput.median - accepted_throughput.median
    required = _required_improvement(
        accepted_throughput,
        candidate_throughput,
        policy.throughput_relative_floor,
    )
    significant = improvement > required
    return ObjectiveComparison(
        candidate_id=candidate.trial_id,
        accepted=significant,
        reason=(
            "candidate significantly increases throughput"
            if significant
            else "throughput improvement is not significant"
        ),
        metric="server_throughput_mpps",
        observed_improvement=improvement,
        required_improvement=required,
        accepted_feasible=True,
        candidate_feasible=True,
    )


def select_historical_best(
    *,
    baseline: ObjectiveTrial,
    accepted_trials: Iterable[ObjectiveTrial],
    policy: ObjectivePolicy,
) -> ObjectiveTrial:
    """Select best from the baseline plus objective-accepted configurations only."""

    history = (baseline, *tuple(accepted_trials))
    if any(trial.status != "valid" for trial in history):
        raise ObjectiveError("historical best may contain only accepted valid trials")
    feasible = tuple(trial for trial in history if _is_feasible(trial, policy))
    if feasible:
        return max(feasible, key=lambda trial: trial.server_throughput.median)
    return min(history, key=lambda trial: trial.client_p999.median)
