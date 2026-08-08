"""Offline steady-state summaries for paper-aligned PipeTune diagnosis."""

from __future__ import annotations

import copy
import dataclasses
import json
import math
import pathlib
import re
import statistics
from typing import Any, Callable

from pipetune.artifacts import (
    load_metric_sample,
    load_session_manifest,
    load_trial_manifest,
    sha256_file,
    write_json_atomic,
)
from pipetune.metrics import AXIO_METRICS_SCHEMA, AxioWindow, load_axio_jsonl
from pipetune.model import (
    ArtifactRef,
    ContractError,
    CounterValue,
    COUNTER_NAMES,
    EndpointSpec,
    FingerprintSet,
    TrialEndpoint,
)


class DiagnosisError(RuntimeError):
    """Raised when immutable trial evidence cannot support diagnosis."""


@dataclasses.dataclass(frozen=True)
class Statistic:
    samples: tuple[float, ...]
    median: float
    mad: float
    uncertainty: float
    unit: str


@dataclasses.dataclass(frozen=True)
class StageComponent:
    name: str
    stage: str
    kind: str
    direction: str
    statistic: Statistic


@dataclasses.dataclass(frozen=True)
class EndpointSteadySummary:
    spec: EndpointSpec
    window_ids: tuple[int, ...]
    throughput: Statistic
    latency: dict[str, Statistic]
    components: tuple[StageComponent, ...]
    supporting: dict[str, Statistic]
    missing_metrics: tuple[str, ...]
    leading_component: StageComponent
    dominant_component: StageComponent | None
    ranking_status: str

    def component(self, name: str) -> StageComponent:
        try:
            return next(component for component in self.components if component.name == name)
        except StopIteration as error:
            raise KeyError(name) from error


@dataclasses.dataclass(frozen=True)
class PeerHealth:
    healthy: bool
    traffic_source: str
    reasons: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class SteadySummary:
    trial_id: str
    target_endpoint_id: str
    target: EndpointSteadySummary
    peer: EndpointSteadySummary
    counters: dict[str, Statistic]
    missing_counters: tuple[str, ...]
    peer_health: PeerHealth
    noise_thresholds: dict[str, float]
    input_hashes: dict[str, str]
    canonical_target: dict[str, Any]
    canonical_peer: dict[str, Any]
    target_fingerprints: FingerprintSet
    peer_fingerprints: FingerprintSet


@dataclasses.dataclass(frozen=True)
class EvidenceItem:
    kind: str
    name: str
    direction: str | None
    value: float | None
    uncertainty: float | None
    unit: str | None
    reason: str


@dataclasses.dataclass(frozen=True)
class ProbeSpec:
    knob: str
    direction: int
    baseline_value: int
    candidate_value: int


@dataclasses.dataclass(frozen=True)
class Diagnosis:
    schema: str
    point: str
    direction: str | None
    confidence: str
    evidence: tuple[EvidenceItem, ...]
    rejected_evidence: tuple[EvidenceItem, ...]
    missing_metrics: tuple[str, ...]
    noise_thresholds: dict[str, float]
    required_probe: ProbeSpec | None
    input_hashes: dict[str, str]
    stage_ranking: tuple[str, ...]
    completed_probe: ProbeSpec | None = None


@dataclasses.dataclass(frozen=True)
class DiagnosisPublication:
    path: pathlib.Path
    document: dict[str, Any]


PIPELINE_STAGES = ("app_tx", "app_rx", "dispatcher_tx", "dispatcher_rx")
NOISE_FIELDS = (
    "throughput_relative_floor",
    "latency_relative_floor",
    "stage_time_relative_floor",
    "stall_time_relative_floor",
    "miss_rate_percentage_point_floor",
)


def _statistic(
    values: tuple[float, ...], *, relative_floor: float, unit: str
) -> Statistic:
    if not values or any(not math.isfinite(value) or value < 0.0 for value in values):
        raise DiagnosisError(f"invalid or empty {unit} sample series")
    median = float(statistics.median(values))
    mad = float(statistics.median(abs(value - median) for value in values))
    return Statistic(
        samples=values,
        median=median,
        mad=mad,
        uncertainty=max(abs(median) * relative_floor, 3.0 * mad),
        unit=unit,
    )


def _absolute_statistic(
    values: tuple[float, ...], *, absolute_floor: float, unit: str
) -> Statistic:
    result = _statistic(values, relative_floor=0.0, unit=unit)
    return dataclasses.replace(
        result,
        uncertainty=max(absolute_floor, 3.0 * result.mad),
    )


def _complete_values(
    windows: tuple[AxioWindow, ...],
    getter: Callable[[AxioWindow], float | None],
) -> tuple[float, ...] | None:
    values = tuple(getter(window) for window in windows)
    if any(value is None for value in values):
        return None
    return tuple(float(value) for value in values if value is not None)


def _stage_direction(stage: str) -> str:
    return "tx" if stage.endswith("_tx") else "rx"


def _endpoint_summary(
    endpoint: TrialEndpoint,
    windows: tuple[AxioWindow, ...],
    noise: dict[str, float],
) -> EndpointSteadySummary:
    throughput_values = _complete_values(
        windows, lambda window: window.e2e_throughput_mpps
    )
    if throughput_values is None:
        raise DiagnosisError(f"{endpoint.spec.endpoint_id} throughput is unavailable")
    throughput = _statistic(
        throughput_values,
        relative_floor=noise["throughput_relative_floor"],
        unit="Mpps",
    )
    latency: dict[str, Statistic] = {}
    missing: list[str] = []
    for name, getter in (
        ("p50_us", lambda item: item.latency_p50_us),
        ("p99_us", lambda item: item.latency_p99_us),
        ("p999_us", lambda item: item.latency_p999_us),
    ):
        values = _complete_values(windows, getter)
        if values is None:
            missing.append(f"latency.{name}")
        else:
            latency[name] = _statistic(
                values,
                relative_floor=noise["latency_relative_floor"],
                unit="us",
            )

    components: list[StageComponent] = []
    for stage in PIPELINE_STAGES:
        for kind, attribute, floor in (
            (
                "completion",
                "completion_time_per_packet_us",
                noise["stage_time_relative_floor"],
            ),
            (
                "stall",
                "stall_time_per_packet_us",
                noise["stall_time_relative_floor"],
            ),
        ):
            values = _complete_values(
                windows,
                lambda window, stage=stage, attribute=attribute: getattr(
                    getattr(window, stage), attribute
                ),
            )
            name = f"{stage}.{kind}"
            if values is None:
                missing.append(name)
                continue
            components.append(
                StageComponent(
                    name=name,
                    stage=stage,
                    kind=kind,
                    direction=_stage_direction(stage),
                    statistic=_statistic(
                        values,
                        relative_floor=floor,
                        unit="us/packet",
                    ),
                )
            )

    nic_tx_values = _complete_values(
        windows, lambda window: window.nic_tx.submit_time_per_packet_us
    )
    if nic_tx_values is None:
        missing.append("nic_tx.submit")
    else:
        components.append(
            StageComponent(
                name="nic_tx.submit",
                stage="nic_tx",
                kind="nic",
                direction="tx",
                statistic=_statistic(
                    nic_tx_values,
                    relative_floor=noise["stage_time_relative_floor"],
                    unit="us/packet",
                ),
            )
        )
    nic_rx_throughput = _complete_values(
        windows, lambda window: window.nic_rx.throughput_mpps
    )
    if nic_rx_throughput is None or any(value <= 0.0 for value in nic_rx_throughput):
        missing.append("nic_rx.aggregate")
    else:
        components.append(
            StageComponent(
                name="nic_rx.aggregate",
                stage="nic_rx",
                kind="nic",
                direction="rx",
                statistic=_statistic(
                    tuple(1.0 / value for value in nic_rx_throughput),
                    relative_floor=noise["stage_time_relative_floor"],
                    unit="us/packet",
                ),
            )
        )

    supporting: dict[str, Statistic] = {}
    for name, getter, unit in (
        (
            "nic_rx.completion_interval_cycles",
            lambda item: item.nic_rx.completion_interval_cycles,
            "cycles",
        ),
        (
            "nic_rx.completion_interval_ns",
            lambda item: item.nic_rx.completion_interval_ns,
            "ns",
        ),
        (
            "nic_rx.slowest_interval_cycles",
            lambda item: item.nic_rx.slowest_interval_cycles,
            "cycles",
        ),
        (
            "nic_rx.capacity_interval_cycles",
            lambda item: item.nic_rx.capacity_interval_cycles,
            "cycles",
        ),
    ):
        values = _complete_values(windows, getter)
        if values is None:
            missing.append(name)
        else:
            supporting[name] = _statistic(
                values,
                relative_floor=noise["stage_time_relative_floor"],
                unit=unit,
            )

    if len(components) < 2:
        raise DiagnosisError(f"{endpoint.spec.endpoint_id} has too few stage metrics")
    ranked = tuple(
        sorted(components, key=lambda component: (-component.statistic.median, component.name))
    )
    leader, runner_up = ranked[:2]
    gap = leader.statistic.median - runner_up.statistic.median
    dominant = (
        leader
        if gap > leader.statistic.uncertainty
        and gap > runner_up.statistic.uncertainty
        else None
    )
    return EndpointSteadySummary(
        spec=endpoint.spec,
        window_ids=tuple(window.window_id for window in windows),
        throughput=throughput,
        latency=latency,
        components=ranked,
        supporting=supporting,
        missing_metrics=tuple(sorted(missing)),
        leading_component=leader,
        dominant_component=dominant,
        ranking_status="dominant" if dominant is not None else "ambiguous",
    )


def _artifact(endpoint: TrialEndpoint, *, path: str | None = None, schema: str | None = None) -> ArtifactRef:
    matches = [
        artifact
        for artifact in endpoint.artifacts
        if (path is None or artifact.path == path)
        and (schema is None or artifact.schema == schema)
    ]
    if len(matches) != 1:
        raise DiagnosisError(
            f"{endpoint.spec.endpoint_id} artifact selection is not unique"
        )
    return matches[0]


def _canonical_config(trial_root: pathlib.Path, endpoint: TrialEndpoint) -> tuple[dict[str, Any], ArtifactRef]:
    reference = _artifact(
        endpoint,
        path=f"configs/canonical/{endpoint.spec.endpoint_id}.json",
    )
    try:
        document = json.loads((trial_root / reference.path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise DiagnosisError("canonical config is not valid JSON") from error
    if not isinstance(document, dict):
        raise DiagnosisError("canonical config must be an object")
    return document, reference


def _policy(document: dict[str, Any]) -> tuple[int, int, dict[str, float]]:
    try:
        tuning = document["tuning"]
        warmup = tuning["warmup_windows"]
        sample = tuning["sample_windows"]
        raw_noise = tuning["noise"]
    except (KeyError, TypeError) as error:
        raise DiagnosisError("canonical config is missing tuning policy") from error
    if type(warmup) is not int or type(sample) is not int or warmup < 0 or sample <= 0:
        raise DiagnosisError("canonical tuning window policy is invalid")
    noise: dict[str, float] = {}
    for field in NOISE_FIELDS:
        try:
            value = raw_noise[field]
        except (KeyError, TypeError) as error:
            raise DiagnosisError(f"canonical tuning noise is missing {field}") from error
        if type(value) not in (int, float) or not math.isfinite(value) or value < 0.0:
            raise DiagnosisError(f"canonical tuning noise {field} is invalid")
        noise[field] = float(value)
    return warmup, sample, noise


def _steady_windows(
    path: pathlib.Path, *, warmup: int, sample: int
) -> tuple[AxioWindow, ...]:
    windows = load_axio_jsonl(path, schema=AXIO_METRICS_SCHEMA)
    if len(windows) < warmup + sample:
        raise DiagnosisError(f"{path}: expected {warmup + sample} windows")
    return windows[warmup : warmup + sample]


def _check_drops(name: str, windows: tuple[AxioWindow, ...]) -> list[str]:
    reasons = []
    if any(window.counters.app_enqueue_drop_count for window in windows):
        reasons.append(f"drop: {name} app enqueue")
    if any(window.counters.dispatcher_enqueue_drop_count for window in windows):
        reasons.append(f"drop: {name} dispatcher enqueue")
    if any(window.counters.nic_rx_completion_error_count for window in windows):
        reasons.append(f"drop: {name} NIC completion error")
    return reasons


def _peer_health(
    target: EndpointSteadySummary,
    peer: EndpointSteadySummary,
    target_windows: tuple[AxioWindow, ...],
    peer_windows: tuple[AxioWindow, ...],
) -> PeerHealth:
    reasons = _check_drops("target", target_windows) + _check_drops(
        "peer", peer_windows
    )
    throughput_gap = abs(target.throughput.median - peer.throughput.median)
    if throughput_gap > target.throughput.uncertainty and throughput_gap > peer.throughput.uncertainty:
        reasons.append(
            "throughput: target/peer median gap exceeds both uncertainties"
        )
    peer_leader = peer.leading_component
    elapsed_gap = (
        peer_leader.statistic.median - target.leading_component.statistic.median
    )
    if (
        peer.dominant_component is not None
        and peer_leader.direction == "tx"
        and elapsed_gap > peer_leader.statistic.uncertainty
        and elapsed_gap > target.leading_component.statistic.uncertainty
    ):
        reasons.append("source: peer traffic-source TX path dominates the target")
    return PeerHealth(
        healthy=not reasons,
        traffic_source="request" if target.spec.role == "server" else "response",
        reasons=tuple(reasons),
    )


def _select_trial(session_root: pathlib.Path, trial_id: str | None) -> tuple[pathlib.Path, ArtifactRef]:
    session_path = session_root / "session.json"
    session = load_session_manifest(session_path, artifact_root=session_root)
    if trial_id is None:
        if len(session.trials) != 1:
            raise DiagnosisError("session contains multiple trials; select --trial")
        selected = session.trials[0]
    else:
        matches = [
            reference
            for reference in session.trials
            if pathlib.PurePosixPath(reference.path).parent.name == trial_id
        ]
        if len(matches) != 1:
            raise DiagnosisError(f"session does not contain trial {trial_id!r}")
        selected = matches[0]
    return session_path, selected


DIRECTION_COUNTERS = {
    "tx": ("llc_store", "io_read"),
    "rx": ("llc_load", "io_write"),
}


def _component_evidence(component: StageComponent, reason: str) -> EvidenceItem:
    return EvidenceItem(
        kind="stage",
        name=component.name,
        direction=component.direction,
        value=component.statistic.median,
        uncertainty=component.statistic.uncertainty,
        unit=component.statistic.unit,
        reason=reason,
    )


def _counter_evidence(
    name: str,
    statistic: Statistic,
    *,
    direction: str,
    reason: str,
) -> EvidenceItem:
    return EvidenceItem(
        kind="counter",
        name=name,
        direction=direction,
        value=statistic.median,
        uncertainty=statistic.uncertainty,
        unit=statistic.unit,
        reason=reason,
    )


def _control_counters(summary: SteadySummary, reason: str) -> tuple[EvidenceItem, ...]:
    direction_by_name = {
        name: direction
        for direction, names in DIRECTION_COUNTERS.items()
        for name in names
    }
    return tuple(
        _counter_evidence(
            name,
            statistic,
            direction=direction_by_name[name],
            reason=reason,
        )
        for name, statistic in sorted(summary.counters.items())
    )


def _directional_counters(
    summary: SteadySummary, direction: str
) -> tuple[tuple[EvidenceItem, ...], tuple[EvidenceItem, ...], tuple[str, ...], str]:
    opposite = "rx" if direction == "tx" else "tx"
    accepted: list[EvidenceItem] = []
    rejected: list[EvidenceItem] = []
    missing = []
    primary_significance: list[bool] = []
    opposite_significant = False
    for name in DIRECTION_COUNTERS[direction]:
        statistic = summary.counters.get(name)
        if statistic is None:
            missing.append(name)
            continue
        significant = statistic.median > statistic.uncertainty
        primary_significance.append(significant)
        item = _counter_evidence(
            name,
            statistic,
            direction=direction,
            reason=(
                "directional counter exceeds its uncertainty"
                if significant
                else "directional counter does not exceed its uncertainty"
            ),
        )
        (accepted if significant else rejected).append(item)
    for name in DIRECTION_COUNTERS[opposite]:
        statistic = summary.counters.get(name)
        if statistic is None:
            missing.append(name)
            continue
        significant = statistic.median > statistic.uncertainty
        opposite_significant = opposite_significant or significant
        rejected.append(
            _counter_evidence(
                name,
                statistic,
                direction=opposite,
                reason=(
                    "conflict: opposite-direction counter is significant"
                    if significant
                    else "opposite-direction control is not significant"
                ),
            )
        )
    primary_conflict = (
        len(primary_significance) == 2
        and primary_significance[0] != primary_significance[1]
    )
    if missing or primary_conflict or opposite_significant:
        confidence = "low"
    elif len(primary_significance) == 2 and all(primary_significance):
        confidence = "high"
    else:
        confidence = "medium"
    return tuple(accepted), tuple(rejected), tuple(sorted(missing)), confidence


def _application_core_probe(summary: SteadySummary) -> ProbeSpec | None:
    try:
        runtime = summary.canonical_target["knobs"]["runtime"]
        topology = summary.canonical_target["deployment"]["topology"]
        current = runtime["application_core_count"]
        dispatcher_count = runtime["dispatcher_queue_count"]
        application_workspaces = topology["application_workspaces"]
    except (KeyError, TypeError) as error:
        raise DiagnosisError("canonical target has no usable C1 topology") from error
    if (
        type(current) is not int
        or type(dispatcher_count) is not int
        or not isinstance(application_workspaces, list)
        or current < 1
        or dispatcher_count < 1
        or current > len(application_workspaces)
    ):
        raise DiagnosisError("canonical target has an invalid C1 topology")
    if current < len(application_workspaces):
        direction = 1
    elif current - 1 >= dispatcher_count:
        direction = -1
    else:
        return None
    return ProbeSpec(
        knob="knobs.runtime.application_core_count",
        direction=direction,
        baseline_value=current,
        candidate_value=current + direction,
    )


def _require_matching_probe(
    baseline: SteadySummary,
    probe: SteadySummary,
    specification: ProbeSpec,
) -> None:
    endpoint_pairs = (
        (baseline.target, probe.target, "target"),
        (baseline.peer, probe.peer, "peer"),
    )
    for before, after, label in endpoint_pairs:
        if before.spec != after.spec:
            raise DiagnosisError(f"{label} endpoint changed during probe")
    target_before = baseline.target_fingerprints
    target_after = probe.target_fingerprints
    if (
        target_before.git_commit != target_after.git_commit
        or target_before.binary_sha256 != target_after.binary_sha256
        or target_before.build != target_after.build
        or target_before.deployment != target_after.deployment
    ):
        raise DiagnosisError("target immutable fingerprint changed during probe")
    if baseline.peer_fingerprints != probe.peer_fingerprints:
        raise DiagnosisError("peer fingerprint changed during target C1 probe")
    try:
        probe_count = probe.canonical_target["knobs"]["runtime"][
            "application_core_count"
        ]
    except (KeyError, TypeError) as error:
        raise DiagnosisError("probe has no application_core_count") from error
    if probe_count != specification.candidate_value:
        raise DiagnosisError(
            "probe application_core_count does not match the requested candidate"
        )
    if _c1_invariant_config(baseline.canonical_target) != _c1_invariant_config(
        probe.canonical_target
    ):
        raise DiagnosisError("target probe changed configuration outside C1 topology")
    if baseline.canonical_peer != probe.canonical_peer:
        raise DiagnosisError("peer configuration changed during target C1 probe")


def _c1_invariant_config(config: dict[str, Any]) -> dict[str, Any]:
    normalized = copy.deepcopy(config)
    try:
        normalized["knobs"]["runtime"]["application_core_count"] = "<C1>"
        topology = normalized["deployment"]["topology"]
    except (KeyError, TypeError) as error:
        raise DiagnosisError("canonical target has no usable C1 topology") from error
    workloads = topology.get("workloads", [])
    if not isinstance(workloads, list):
        raise DiagnosisError("canonical target workloads must be an array")
    for workload in workloads:
        if not isinstance(workload, dict):
            raise DiagnosisError("canonical target workload must be an object")
        groups = workload.get("groups", [])
        if not isinstance(groups, list):
            raise DiagnosisError("canonical target workload groups must be an array")
        for group in groups:
            if not isinstance(group, dict):
                raise DiagnosisError("canonical target workload group must be an object")
            if "applications" in group:
                group["applications"] = "<C1-derived>"
    return normalized


def _probe_counter_evidence(
    label: str,
    name: str,
    statistic: Statistic,
    *,
    direction: str,
    reason: str,
) -> EvidenceItem:
    return _counter_evidence(
        f"{label}.{name}", statistic, direction=direction, reason=reason
    )


def _combined_probe_hashes(
    baseline: SteadySummary, probe: SteadySummary
) -> dict[str, str]:
    hashes = {
        f"baseline:{name}": value
        for name, value in baseline.input_hashes.items()
    }
    hashes.update(
        {f"probe:{name}": value for name, value in probe.input_hashes.items()}
    )
    return dict(sorted(hashes.items()))


def _classify_core_probe(
    summary: SteadySummary,
    probe_summary: SteadySummary,
    dominant: StageComponent,
    specification: ProbeSpec,
) -> Diagnosis:
    _require_matching_probe(summary, probe_summary, specification)
    direction = dominant.direction
    opposite = "rx" if direction == "tx" else "tx"
    llc_name, io_name = DIRECTION_COUNTERS[direction]
    _, opposite_io_name = DIRECTION_COUNTERS[opposite]
    required_names = (llc_name, io_name, opposite_io_name)
    missing = {
        *(f"baseline.{name}" for name in summary.missing_counters),
        *(f"probe.{name}" for name in probe_summary.missing_counters),
    }
    for name in required_names:
        if name not in summary.counters:
            missing.add(f"baseline.{name}")
        if name not in probe_summary.counters:
            missing.add(f"probe.{name}")

    evidence: list[EvidenceItem] = [
        _component_evidence(dominant, "dominant completion requires a C1 perturbation")
    ]
    rejected: list[EvidenceItem] = []
    for label, candidate in (("baseline", summary), ("probe", probe_summary)):
        for counter_name, counter in sorted(candidate.counters.items()):
            counter_direction = next(
                key
                for key, names in DIRECTION_COUNTERS.items()
                if counter_name in names
            )
            significant = counter.median > counter.uncertainty
            is_opposite = counter_direction != direction
            item = _probe_counter_evidence(
                label,
                counter_name,
                counter,
                direction=counter_direction,
                reason=(
                    "conflict: opposite-direction counter exceeds its uncertainty"
                    if significant and is_opposite
                    else "counter exceeds its uncertainty"
                    if significant
                    else "counter does not exceed its uncertainty"
                ),
            )
            destination = (
                evidence
                if counter_name in (llc_name, io_name) and significant
                else rejected
            )
            destination.append(item)

    base = dict(
        schema="pipetune.diagnosis/v1",
        direction=direction,
        evidence=tuple(evidence),
        rejected_evidence=tuple(rejected),
        missing_metrics=tuple(sorted(missing)),
        noise_thresholds=summary.noise_thresholds,
        required_probe=None,
        input_hashes=_combined_probe_hashes(summary, probe_summary),
        stage_ranking=tuple(component.name for component in summary.target.components),
        completed_probe=specification,
    )
    if not probe_summary.peer_health.healthy:
        health_evidence = tuple(
            EvidenceItem(
                kind="health",
                name="probe_peer_unhealthy",
                direction=None,
                value=None,
                uncertainty=None,
                unit=None,
                reason=reason,
            )
            for reason in probe_summary.peer_health.reasons
        )
        return dataclasses.replace(
            Diagnosis(point="peer_unhealthy", confidence="none", **base),
            evidence=health_evidence,
            rejected_evidence=(
                *base["evidence"],
                *base["rejected_evidence"],
            ),
        )
    if llc_name not in summary.counters or llc_name not in probe_summary.counters:
        return Diagnosis(point="inconclusive", confidence="none", **base)

    baseline_llc = summary.counters[llc_name]
    probe_llc = probe_summary.counters[llc_name]
    slope_uncertainty = max(
        summary.noise_thresholds["miss_rate_percentage_point_floor"],
        3.0 * baseline_llc.mad,
        3.0 * probe_llc.mad,
    )
    slope = (probe_llc.median - baseline_llc.median) * specification.direction
    slope_item = EvidenceItem(
        kind="perturbation",
        name="llc_slope",
        direction=direction,
        value=slope,
        uncertainty=slope_uncertainty,
        unit="percentage points per core",
        reason="LLC miss-rate change normalized to increasing C1",
    )
    if slope > slope_uncertainty:
        directional_io_significant = all(
            io_name in candidate.counters
            and candidate.counters[io_name].median
            > candidate.counters[io_name].uncertainty
            for candidate in (summary, probe_summary)
        )
        opposite_llc_name, _ = DIRECTION_COUNTERS[opposite]
        opposite_llc_significant = any(
            opposite_llc_name in candidate.counters
            and candidate.counters[opposite_llc_name].median
            > candidate.counters[opposite_llc_name].uncertainty
            for candidate in (summary, probe_summary)
        )
        opposite_io_significant = any(
            opposite_io_name in candidate.counters
            and candidate.counters[opposite_io_name].median
            > candidate.counters[opposite_io_name].uncertainty
            for candidate in (summary, probe_summary)
        )
        if opposite_io_significant:
            confidence = "low"
        elif missing or opposite_llc_significant or not directional_io_significant:
            confidence = "medium"
        else:
            confidence = "high"
        return dataclasses.replace(
            Diagnosis(
                point="P2",
                confidence=confidence,
                **base,
            ),
            evidence=(*base["evidence"], slope_item),
        )

    rejected.append(slope_item)
    if any(
        name not in summary.counters or name not in probe_summary.counters
        for name in (io_name, opposite_io_name)
    ):
        return dataclasses.replace(
            Diagnosis(point="inconclusive", confidence="none", **base),
            rejected_evidence=tuple(rejected),
        )
    io_significant = all(
        candidate.counters[io_name].median > candidate.counters[io_name].uncertainty
        for candidate in (summary, probe_summary)
    )
    opposite_io_significant = any(
        candidate.counters[opposite_io_name].median
        > candidate.counters[opposite_io_name].uncertainty
        for candidate in (summary, probe_summary)
    )
    point = "P4" if io_significant and not opposite_io_significant else "inconclusive"
    opposite_llc_name, _ = DIRECTION_COUNTERS[opposite]
    opposite_llc_significant = any(
        opposite_llc_name in candidate.counters
        and candidate.counters[opposite_llc_name].median
        > candidate.counters[opposite_llc_name].uncertainty
        for candidate in (summary, probe_summary)
    )
    if point != "P4":
        confidence = "none"
    elif missing or opposite_llc_significant:
        confidence = "medium"
    else:
        confidence = "high"
    return dataclasses.replace(
        Diagnosis(point=point, confidence=confidence, **base),
        rejected_evidence=tuple(rejected),
    )


def diagnose_summary(
    summary: SteadySummary, *, probe_summary: SteadySummary | None = None
) -> Diagnosis:
    """Apply the paper's P1--P4 longest-component decision tree."""

    ranking = tuple(component.name for component in summary.target.components)
    base = dict(
        schema="pipetune.diagnosis/v1",
        noise_thresholds=summary.noise_thresholds,
        input_hashes=summary.input_hashes,
        stage_ranking=ranking,
    )
    if not summary.peer_health.healthy:
        evidence = tuple(
            EvidenceItem(
                kind="health",
                name="peer_unhealthy",
                direction=None,
                value=None,
                uncertainty=None,
                unit=None,
                reason=reason,
            )
            for reason in summary.peer_health.reasons
        )
        return Diagnosis(
            point="peer_unhealthy",
            direction=None,
            confidence="none",
            evidence=evidence,
            rejected_evidence=_control_counters(
                summary, "target diagnosis rejected by peer health gate"
            ),
            missing_metrics=summary.missing_counters,
            required_probe=None,
            **base,
        )
    dominant = summary.target.dominant_component
    if dominant is None:
        return Diagnosis(
            point="inconclusive",
            direction=None,
            confidence="none",
            evidence=(),
            rejected_evidence=(
                _component_evidence(
                    summary.target.leading_component,
                    "stage gap does not exceed both uncertainties",
                ),
                *_control_counters(
                    summary, "no dominant direction for counter interpretation"
                ),
            ),
            missing_metrics=tuple(
                sorted((*summary.target.missing_metrics, *summary.missing_counters))
            ),
            required_probe=None,
            **base,
        )
    if dominant.kind == "stall":
        point = "P1"
    elif dominant.kind == "nic":
        point = "P3"
    else:
        probe = _application_core_probe(summary)
        if probe is None:
            return Diagnosis(
                point="inconclusive",
                direction=dominant.direction,
                confidence="none",
                evidence=(
                    _component_evidence(
                        dominant, "dominant completion has no legal C1 perturbation"
                    ),
                ),
                rejected_evidence=_control_counters(
                    summary, "P2/P4 requires a legal C1 perturbation"
                ),
                missing_metrics=tuple(
                    sorted((*summary.missing_counters, "c1_probe_capacity"))
                ),
                required_probe=None,
                **base,
            )
        if probe_summary is not None:
            return _classify_core_probe(summary, probe_summary, dominant, probe)
        return Diagnosis(
            point="probe_required",
            direction=dominant.direction,
            confidence="medium",
            evidence=(
                _component_evidence(
                    dominant, "dominant completion requires a C1 perturbation"
                ),
            ),
            rejected_evidence=_control_counters(
                summary, "P2/P4 requires perturbation before counter classification"
            ),
            missing_metrics=summary.missing_counters,
            required_probe=probe,
            **base,
        )
    accepted, rejected, missing, confidence = _directional_counters(
        summary, dominant.direction
    )
    return Diagnosis(
        point=point,
        direction=dominant.direction,
        confidence=confidence,
        evidence=(
            _component_evidence(
                dominant,
                "dominant elapsed component exceeds both ranking uncertainties",
            ),
            *accepted,
        ),
        rejected_evidence=rejected,
        missing_metrics=tuple(
            sorted((*summary.target.missing_metrics, *missing))
        ),
        required_probe=None,
        **base,
    )


def _summarize_trial(
    trial_path: pathlib.Path, input_hashes: dict[str, str]
) -> SteadySummary:
    try:
        trial_root = trial_path.parent
        manifest = load_trial_manifest(trial_path, artifact_root=trial_root)
        endpoint_by_id = {
            endpoint.spec.endpoint_id: endpoint for endpoint in manifest.endpoints
        }
        target_endpoint = endpoint_by_id[manifest.target_endpoint_id]
        peer_endpoint = next(
            endpoint
            for endpoint in manifest.endpoints
            if endpoint.spec.endpoint_id != manifest.target_endpoint_id
        )
        target_config, target_config_ref = _canonical_config(
            trial_root, target_endpoint
        )
        peer_config, peer_config_ref = _canonical_config(trial_root, peer_endpoint)
        warmup, sample, noise = _policy(target_config)
        if _policy(peer_config) != (warmup, sample, noise):
            raise DiagnosisError("target and peer tuning policies differ")

        for endpoint in manifest.endpoints:
            source = _artifact(
                endpoint,
                path=f"configs/source/{endpoint.spec.endpoint_id}.toml",
            )
            if source.sha256 != endpoint.fingerprints.source_config_sha256:
                raise DiagnosisError(
                    f"{endpoint.spec.endpoint_id} source fingerprint mismatch"
                )
            input_hashes[source.path] = source.sha256
            input_hashes[endpoint.process.stdout.path] = endpoint.process.stdout.sha256
            input_hashes[endpoint.process.stderr.path] = endpoint.process.stderr.sha256
            for reference in endpoint.artifacts:
                input_hashes[reference.path] = reference.sha256
        input_hashes[target_config_ref.path] = target_config_ref.sha256
        input_hashes[peer_config_ref.path] = peer_config_ref.sha256

        target_metrics = _artifact(target_endpoint, schema=AXIO_METRICS_SCHEMA)
        peer_metrics = _artifact(peer_endpoint, schema=AXIO_METRICS_SCHEMA)
        target_windows = _steady_windows(
            trial_root / target_metrics.path,
            warmup=warmup,
            sample=sample,
        )
        peer_windows = _steady_windows(
            trial_root / peer_metrics.path,
            warmup=warmup,
            sample=sample,
        )
        target = _endpoint_summary(target_endpoint, target_windows, noise)
        peer = _endpoint_summary(peer_endpoint, peer_windows, noise)

        if manifest.host_metrics is None:
            raise DiagnosisError("trial has no target host metrics")
        input_hashes[manifest.host_metrics.path] = manifest.host_metrics.sha256
        host = load_metric_sample(
            trial_root / manifest.host_metrics.path,
            artifact_root=trial_root,
        )
        if host.endpoint_id != manifest.target_endpoint_id:
            raise DiagnosisError("host metrics belong to a non-target endpoint")
        for reference in host.raw_artifacts:
            input_hashes[reference.path] = reference.sha256
        counters: dict[str, Statistic] = {}
        missing_counters = []
        for counter in host.counters:
            if not counter.available or counter.rate_percent is None:
                missing_counters.append(counter.name)
                continue
            values = counter.samples_percent or (counter.rate_percent,)
            counters[counter.name] = _absolute_statistic(
                tuple(values),
                absolute_floor=noise["miss_rate_percentage_point_floor"],
                unit="percentage points",
            )
        return SteadySummary(
            trial_id=manifest.trial_id,
            target_endpoint_id=manifest.target_endpoint_id,
            target=target,
            peer=peer,
            counters=counters,
            missing_counters=tuple(sorted(missing_counters)),
            peer_health=_peer_health(target, peer, target_windows, peer_windows),
            noise_thresholds=noise,
            input_hashes=dict(sorted(input_hashes.items())),
            canonical_target=target_config,
            canonical_peer=peer_config,
            target_fingerprints=target_endpoint.fingerprints,
            peer_fingerprints=peer_endpoint.fingerprints,
        )
    except DiagnosisError:
        raise
    except (ContractError, OSError, KeyError, StopIteration, ValueError) as error:
        raise DiagnosisError(str(error)) from error


def summarize_trial(trial_path: pathlib.Path) -> SteadySummary:
    """Load and summarize one immutable trial without an E8 session wrapper."""

    try:
        resolved = trial_path.resolve(strict=True)
        if not resolved.is_file():
            raise DiagnosisError(f"trial manifest is not a file: {resolved}")
        relative = f"trials/{resolved.parent.name}/{resolved.name}"
        return _summarize_trial(
            resolved,
            {relative: sha256_file(resolved)},
        )
    except DiagnosisError:
        raise
    except (OSError, UnicodeError, ValueError) as error:
        raise DiagnosisError(str(error)) from error


def summarize_session(
    session_root: pathlib.Path, *, trial_id: str | None = None
) -> SteadySummary:
    """Load, verify, and independently summarize one immutable E8 trial."""

    try:
        session_root = session_root.resolve(strict=True)
        session_path, trial_reference = _select_trial(session_root, trial_id)
        return _summarize_trial(
            session_root / trial_reference.path,
            {
                "session.json": sha256_file(session_path),
                trial_reference.path: trial_reference.sha256,
            },
        )
    except DiagnosisError:
        raise
    except (ContractError, OSError, KeyError, StopIteration, ValueError) as error:
        raise DiagnosisError(str(error)) from error


def _statistic_document(statistic: Statistic) -> dict[str, object]:
    return {
        "mad": statistic.mad,
        "median": statistic.median,
        "sample_count": len(statistic.samples),
        "uncertainty": statistic.uncertainty,
        "unit": statistic.unit,
    }


def _component_document(component: StageComponent) -> dict[str, object]:
    return {
        "direction": component.direction,
        "kind": component.kind,
        "name": component.name,
        "stage": component.stage,
        "statistic": _statistic_document(component.statistic),
    }


def _evidence_document(item: EvidenceItem) -> dict[str, object]:
    return {
        "direction": item.direction,
        "kind": item.kind,
        "name": item.name,
        "reason": item.reason,
        "uncertainty": item.uncertainty,
        "unit": item.unit,
        "value": item.value,
    }


def _probe_document(probe: ProbeSpec | None) -> dict[str, object] | None:
    if probe is None:
        return None
    return {
        "baseline_value": probe.baseline_value,
        "candidate_value": probe.candidate_value,
        "direction": probe.direction,
        "knob": probe.knob,
    }


def _counter_vector(summary: SteadySummary) -> dict[str, object]:
    return {
        name: (
            _statistic_document(summary.counters[name])
            if name in summary.counters
            else None
        )
        for name in COUNTER_NAMES
    }


def diagnosis_document(
    diagnosis: Diagnosis,
    summary: SteadySummary,
    *,
    probe_summary: SteadySummary | None = None,
) -> dict[str, object]:
    """Build a compact deterministic document from immutable diagnosis inputs."""

    confidence_reasons = tuple(
        dict.fromkeys(
            item.reason
            for item in (*diagnosis.evidence, *diagnosis.rejected_evidence)
        )
    )
    ranked = tuple(
        summary.target.component(name) for name in diagnosis.stage_ranking
    )
    return {
        "counter_rates": {
            "baseline": _counter_vector(summary),
            "probe": _counter_vector(probe_summary) if probe_summary else None,
        },
        "input_hashes": diagnosis.input_hashes,
        "noise_thresholds": diagnosis.noise_thresholds,
        "result": {
            "completed_probe": _probe_document(diagnosis.completed_probe),
            "confidence": diagnosis.confidence,
            "confidence_reasons": list(confidence_reasons),
            "direction": diagnosis.direction,
            "evidence": [
                _evidence_document(item) for item in diagnosis.evidence
            ],
            "missing_metrics": list(diagnosis.missing_metrics),
            "point": diagnosis.point,
            "rejected_evidence": [
                _evidence_document(item) for item in diagnosis.rejected_evidence
            ],
            "required_probe": _probe_document(diagnosis.required_probe),
        },
        "schema": diagnosis.schema,
        "steady_state": {
            "peer": {
                "endpoint_id": summary.peer.spec.endpoint_id,
                "health": {
                    "healthy": summary.peer_health.healthy,
                    "reasons": list(summary.peer_health.reasons),
                    "traffic_source": summary.peer_health.traffic_source,
                },
                "throughput": _statistic_document(summary.peer.throughput),
            },
            "target": {
                "endpoint_id": summary.target.spec.endpoint_id,
                "latency": {
                    name: _statistic_document(statistic)
                    for name, statistic in sorted(summary.target.latency.items())
                },
                "stage_ranking": [
                    _component_document(component) for component in ranked
                ],
                "supporting": {
                    name: _statistic_document(statistic)
                    for name, statistic in sorted(summary.target.supporting.items())
                },
                "throughput": _statistic_document(summary.target.throughput),
                "window_ids": list(summary.target.window_ids),
            },
        },
        "trial": {
            "baseline": summary.trial_id,
            "probe": probe_summary.trial_id if probe_summary else None,
        },
    }


def _safe_trial_name(trial_id: str) -> str:
    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", trial_id) is None:
        raise DiagnosisError(f"trial id {trial_id!r} is unsafe for an output path")
    return trial_id


def publish_diagnosis(
    session_root: pathlib.Path,
    *,
    trial_id: str | None = None,
    probe_session_root: pathlib.Path | None = None,
    probe_trial_id: str | None = None,
) -> DiagnosisPublication:
    """Diagnose local E8 artifacts and atomically publish derived JSON."""

    if probe_trial_id is not None and probe_session_root is None:
        raise DiagnosisError("--probe-trial requires --probe-session")
    summary = summarize_session(session_root, trial_id=trial_id)
    diagnosis = diagnose_summary(summary)
    probe_summary = None
    if probe_session_root is not None:
        if diagnosis.point != "probe_required":
            raise DiagnosisError(
                f"probe supplied but baseline does not require one ({diagnosis.point})"
            )
        probe_summary = summarize_session(
            probe_session_root, trial_id=probe_trial_id
        )
        diagnosis = diagnose_summary(summary, probe_summary=probe_summary)
    document = diagnosis_document(
        diagnosis, summary, probe_summary=probe_summary
    )
    baseline_name = _safe_trial_name(summary.trial_id)
    filename = baseline_name
    if probe_summary is not None:
        filename += f"--probe-{_safe_trial_name(probe_summary.trial_id)}"
    root = session_root.resolve(strict=True)
    output = root / "diagnoses" / f"{filename}.json"
    write_json_atomic(output, document)
    return DiagnosisPublication(path=output, document=document)


__all__ = [
    "Diagnosis",
    "DiagnosisError",
    "DiagnosisPublication",
    "EvidenceItem",
    "EndpointSteadySummary",
    "PeerHealth",
    "ProbeSpec",
    "StageComponent",
    "Statistic",
    "SteadySummary",
    "diagnose_summary",
    "diagnosis_document",
    "publish_diagnosis",
    "summarize_session",
    "summarize_trial",
]
