"""Offline steady-state summaries for paper-aligned PipeTune diagnosis."""

from __future__ import annotations

import dataclasses
import json
import math
import pathlib
import statistics
from typing import Any, Callable

from pipetune.artifacts import (
    load_metric_sample,
    load_session_manifest,
    load_trial_manifest,
    sha256_file,
)
from pipetune.metrics import AXIO_METRICS_SCHEMA, AxioWindow, load_axio_jsonl
from pipetune.model import (
    ArtifactRef,
    ContractError,
    CounterValue,
    EndpointSpec,
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


def summarize_session(
    session_root: pathlib.Path, *, trial_id: str | None = None
) -> SteadySummary:
    """Load, verify, and independently summarize one immutable E8 trial."""

    try:
        session_root = session_root.resolve(strict=True)
        session_path, trial_reference = _select_trial(session_root, trial_id)
        trial_path = session_root / trial_reference.path
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

        input_hashes = {
            "session.json": sha256_file(session_path),
            trial_reference.path: trial_reference.sha256,
        }
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
        )
    except DiagnosisError:
        raise
    except (ContractError, OSError, KeyError, StopIteration, ValueError) as error:
        raise DiagnosisError(str(error)) from error


__all__ = [
    "DiagnosisError",
    "EndpointSteadySummary",
    "PeerHealth",
    "StageComponent",
    "Statistic",
    "SteadySummary",
    "summarize_session",
]
