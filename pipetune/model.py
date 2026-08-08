"""Immutable PipeTune data contracts shared by collection and tuning."""

from __future__ import annotations

import dataclasses
import datetime as dt
import math
import pathlib
import re


SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
GIT_SHA_PATTERN = re.compile(r"^[0-9a-f]{40}(?:[0-9a-f]{24})?$")
COUNTER_NAMES = ("llc_load", "llc_store", "io_read", "io_write")
PROVIDER_NAMES = ("perf", "pcm_pcie")
ARTIFACT_SCHEMAS = (
    "axio.metrics/v1",
    "pipetune.host-metrics/v1",
    "pipetune.session/v1",
    "pipetune.trial/v1",
)


class ContractError(ValueError):
    """Raised when persisted PipeTune data violates a versioned contract."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def _require_string(value: object, field: str, *, allow_empty: bool = False) -> str:
    _require(isinstance(value, str), f"{field} must be a string")
    if not allow_empty:
        _require(bool(value), f"{field} must not be empty")
    return value


def _require_int(value: object, field: str, *, minimum: int | None = None) -> int:
    _require(type(value) is int, f"{field} must be an integer")
    if minimum is not None:
        _require(value >= minimum, f"{field} must be at least {minimum}")
    return value


def _require_number(
    value: object, field: str, *, minimum: float | None = None
) -> float:
    _require(type(value) in (int, float), f"{field} must be a number")
    number = float(value)
    _require(math.isfinite(number), f"{field} must be finite")
    if minimum is not None:
        _require(number >= minimum, f"{field} must be at least {minimum}")
    return number


def _timestamp(value: str, field: str) -> dt.datetime:
    _require_string(value, field)
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as error:
        raise ContractError(f"{field} must be an ISO-8601 timestamp") from error
    _require(parsed.tzinfo is not None, f"{field} must include a UTC offset")
    _require(parsed.utcoffset() == dt.timedelta(0), f"{field} must be UTC")
    return parsed


@dataclasses.dataclass(frozen=True)
class ArtifactRef:
    path: str
    sha256: str
    size_bytes: int
    schema: str | None = None

    def __post_init__(self) -> None:
        _require_string(self.path, "artifact.path")
        path = pathlib.PurePosixPath(self.path)
        _require(not path.is_absolute(), "artifact.path must be relative")
        _require(".." not in path.parts, "artifact.path must remain contained")
        _require(
            bool(SHA256_PATTERN.fullmatch(self.sha256)),
            "artifact.sha256 must be lowercase SHA-256",
        )
        _require_int(self.size_bytes, "artifact.size_bytes", minimum=0)
        if self.schema is not None:
            _require(
                self.schema in ARTIFACT_SCHEMAS,
                f"unsupported artifact schema {self.schema!r}",
            )


@dataclasses.dataclass(frozen=True)
class EndpointSpec:
    endpoint_id: str
    role: str
    backend: str
    transport: str
    host: str
    ssh_port: int
    ssh_user: str
    workdir: str
    use_sudo: bool
    numa_node: int

    def __post_init__(self) -> None:
        _require_string(self.endpoint_id, "endpoint.endpoint_id")
        _require(self.role in ("client", "server"), "endpoint.role is unsupported")
        _require(self.backend in ("dpdk", "roce"), "endpoint.backend is unsupported")
        _require(
            self.transport in ("local", "ssh"),
            "endpoint.transport is unsupported",
        )
        _require_string(self.host, "endpoint.host", allow_empty=True)
        _require_int(self.ssh_port, "endpoint.ssh_port", minimum=0)
        _require_string(self.ssh_user, "endpoint.ssh_user", allow_empty=True)
        _require_string(self.workdir, "endpoint.workdir")
        _require(type(self.use_sudo) is bool, "endpoint.use_sudo must be a boolean")
        _require_int(self.numa_node, "endpoint.numa_node", minimum=0)
        if self.transport == "ssh":
            _require(bool(self.host), "SSH endpoint.host must not be empty")
            _require(bool(self.ssh_user), "SSH endpoint.ssh_user must not be empty")
            _require(
                1 <= self.ssh_port <= 65535,
                "SSH endpoint.ssh_port must be between 1 and 65535",
            )


@dataclasses.dataclass(frozen=True)
class FingerprintSet:
    git_commit: str
    binary_sha256: str
    source_config_sha256: str
    build: str
    datapath: str
    deployment: str

    def __post_init__(self) -> None:
        _require(
            bool(GIT_SHA_PATTERN.fullmatch(self.git_commit)),
            "fingerprints.git_commit must be a full Git SHA",
        )
        for field, value in (
            ("binary_sha256", self.binary_sha256),
            ("source_config_sha256", self.source_config_sha256),
        ):
            _require(
                bool(SHA256_PATTERN.fullmatch(value)),
                f"fingerprints.{field} must be lowercase SHA-256",
            )
        _require_string(self.build, "fingerprints.build")
        _require_string(self.datapath, "fingerprints.datapath")
        _require_string(self.deployment, "fingerprints.deployment")


@dataclasses.dataclass(frozen=True)
class ProcessResult:
    argv: tuple[str, ...]
    status: str
    return_code: int | None
    failure_reason: str | None
    started_at_utc: str
    ended_at_utc: str
    stdout: ArtifactRef
    stderr: ArtifactRef

    def __post_init__(self) -> None:
        _require(bool(self.argv), "process.argv must not be empty")
        for index, argument in enumerate(self.argv):
            _require_string(argument, f"process.argv[{index}]", allow_empty=True)
        _require(
            self.status in ("exited", "timed_out", "transport_error", "not_started"),
            "process.status is unsupported",
        )
        if self.status == "exited":
            _require(
                self.return_code is not None,
                "exited process must have a return code",
            )
            _require_int(self.return_code, "process.return_code")
            _require(
                self.failure_reason is None,
                "exited process must not have a failure reason",
            )
        else:
            _require(
                self.return_code is None,
                "unfinished process must not fabricate a return code",
            )
            _require_string(self.failure_reason, "process.failure_reason")
        started = _timestamp(self.started_at_utc, "process.started_at_utc")
        ended = _timestamp(self.ended_at_utc, "process.ended_at_utc")
        _require(ended >= started, "process timestamps are reversed")


@dataclasses.dataclass(frozen=True)
class CounterValue:
    name: str
    available: bool
    numerator: float | None
    denominator: float | None
    rate_percent: float | None
    reason: str | None
    samples_percent: tuple[float, ...] = ()

    def __post_init__(self) -> None:
        _require(self.name in COUNTER_NAMES, f"unsupported counter {self.name!r}")
        _require(type(self.available) is bool, "counter.available must be a boolean")
        if self.available:
            _require(self.numerator is not None, "available counter needs numerator")
            _require(self.denominator is not None, "available counter needs denominator")
            _require(self.rate_percent is not None, "available counter needs rate")
            numerator = _require_number(
                self.numerator, "counter.numerator", minimum=0.0
            )
            denominator = _require_number(
                self.denominator, "counter.denominator", minimum=0.0
            )
            _require(denominator > 0.0, "counter.denominator must be positive")
            rate = _require_number(
                self.rate_percent, "counter.rate_percent", minimum=0.0
            )
            _require(rate <= 100.0, "counter.rate_percent must not exceed 100")
            expected = numerator / denominator * 100.0
            _require(
                abs(rate - expected) <= 0.01,
                "counter.rate_percent does not match numerator/denominator",
            )
            _require(self.reason is None, "available counter must not have a reason")
            for index, sample in enumerate(self.samples_percent):
                value = _require_number(
                    sample,
                    f"counter.samples_percent[{index}]",
                    minimum=0.0,
                )
                _require(
                    value <= 100.0,
                    "counter sample rate must not exceed 100",
                )
        else:
            _require(
                self.numerator is None
                and self.denominator is None
                and self.rate_percent is None,
                "unavailable counter values must be null",
            )
            _require_string(self.reason, "counter.reason")
            _require(
                not self.samples_percent,
                "unavailable counter samples must be empty",
            )


@dataclasses.dataclass(frozen=True)
class ProviderStatus:
    name: str
    available: bool
    path: str | None
    version: str | None
    reason: str | None

    def __post_init__(self) -> None:
        _require_string(self.name, "provider.name")
        _require(type(self.available) is bool, "provider.available must be a boolean")
        if self.path is not None:
            _require_string(self.path, "provider.path")
        if self.version is not None:
            _require_string(self.version, "provider.version")
        if self.available:
            _require(self.path is not None, "available provider needs a path")
            _require(self.version is not None, "available provider needs a version")
            _require(self.reason is None, "available provider must not have a reason")
        else:
            _require_string(self.reason, "provider.reason")


@dataclasses.dataclass(frozen=True)
class MetricSample:
    schema: str
    sample_id: str
    endpoint_id: str
    started_at_utc: str
    ended_at_utc: str
    sample_interval_seconds: float
    socket_id: int
    counters: tuple[CounterValue, ...]
    providers: tuple[ProviderStatus, ...]
    commands: tuple[ProcessResult, ...]
    raw_artifacts: tuple[ArtifactRef, ...]

    def __post_init__(self) -> None:
        _require(
            self.schema == "pipetune.host-metrics/v1",
            "unsupported host metrics schema",
        )
        _require_string(self.sample_id, "host_metrics.sample_id")
        _require_string(self.endpoint_id, "host_metrics.endpoint_id")
        started = _timestamp(self.started_at_utc, "host_metrics.started_at_utc")
        ended = _timestamp(self.ended_at_utc, "host_metrics.ended_at_utc")
        _require(ended >= started, "host metrics timestamps are reversed")
        _require(
            _require_number(
                self.sample_interval_seconds,
                "host_metrics.sample_interval_seconds",
                minimum=0.0,
            )
            > 0.0,
            "host_metrics.sample_interval_seconds must be positive",
        )
        _require_int(self.socket_id, "host_metrics.socket_id", minimum=0)
        names = tuple(counter.name for counter in self.counters)
        _require(names == COUNTER_NAMES, "host metrics must contain four ordered counters")
        provider_names = tuple(provider.name for provider in self.providers)
        _require(
            provider_names == PROVIDER_NAMES,
            "host metrics must contain ordered perf and pcm_pcie providers",
        )
        artifact_paths = tuple(artifact.path for artifact in self.raw_artifacts)
        _require(
            len(artifact_paths) == len(set(artifact_paths)),
            "host metrics raw artifacts must be unique",
        )
        raw_paths = set(artifact_paths)
        for command in self.commands:
            _require(
                command.stdout.path in raw_paths and command.stderr.path in raw_paths,
                "host metric command output must be preserved as a raw artifact",
            )


@dataclasses.dataclass(frozen=True)
class TrialEndpoint:
    spec: EndpointSpec
    fingerprints: FingerprintSet
    process: ProcessResult
    artifacts: tuple[ArtifactRef, ...]

    def __post_init__(self) -> None:
        paths = [artifact.path for artifact in self.artifacts]
        _require(len(paths) == len(set(paths)), "endpoint artifacts must be unique")


@dataclasses.dataclass(frozen=True)
class TrialManifest:
    schema: str
    trial_id: str
    target_endpoint_id: str
    started_at_utc: str
    ended_at_utc: str
    status: str
    cleanup_status: str
    endpoints: tuple[TrialEndpoint, ...]
    host_metrics: ArtifactRef | None
    failure_reason: str | None

    def __post_init__(self) -> None:
        _require(self.schema == "pipetune.trial/v1", "unsupported trial schema")
        _require_string(self.trial_id, "trial.trial_id")
        _require_string(self.target_endpoint_id, "trial.target_endpoint_id")
        started = _timestamp(self.started_at_utc, "trial.started_at_utc")
        ended = _timestamp(self.ended_at_utc, "trial.ended_at_utc")
        _require(ended >= started, "trial timestamps are reversed")
        _require(self.status in ("success", "failed"), "trial.status is unsupported")
        _require(
            self.cleanup_status in ("clean", "failed"),
            "trial.cleanup_status is unsupported",
        )
        _require(len(self.endpoints) == 2, "trial must contain exactly two endpoints")
        endpoint_ids = [endpoint.spec.endpoint_id for endpoint in self.endpoints]
        _require(len(set(endpoint_ids)) == 2, "trial endpoint IDs must be unique")
        _require(
            self.target_endpoint_id in endpoint_ids,
            "trial target endpoint is missing",
        )
        _require(
            {endpoint.spec.role for endpoint in self.endpoints}
            == {"client", "server"},
            "trial must contain one client and one server",
        )
        _require(
            len({endpoint.spec.backend for endpoint in self.endpoints}) == 1,
            "trial endpoint backends must match",
        )
        if self.status == "success":
            _require(self.failure_reason is None, "successful trial has failure reason")
            _require(self.cleanup_status == "clean", "successful trial cleanup failed")
            _require(
                self.host_metrics is not None
                and self.host_metrics.schema == "pipetune.host-metrics/v1",
                "successful trial requires typed host metrics",
            )
            for endpoint in self.endpoints:
                _require(
                    endpoint.process.return_code == 0
                    and endpoint.process.status == "exited",
                    "successful trial contains a failed endpoint",
                )
                _require(
                    any(
                        artifact.schema == "axio.metrics/v1"
                        for artifact in endpoint.artifacts
                    ),
                    "successful trial endpoint requires Axio metrics",
                )
        else:
            _require_string(self.failure_reason, "trial.failure_reason")


@dataclasses.dataclass(frozen=True)
class SessionManifest:
    schema: str
    session_id: str
    started_at_utc: str
    ended_at_utc: str | None
    status: str
    trials: tuple[ArtifactRef, ...]
    failure_reason: str | None

    def __post_init__(self) -> None:
        _require(self.schema == "pipetune.session/v1", "unsupported session schema")
        _require_string(self.session_id, "session.session_id")
        started = _timestamp(self.started_at_utc, "session.started_at_utc")
        _require(
            self.status in ("running", "complete", "failed"),
            "session.status is unsupported",
        )
        paths = tuple(trial.path for trial in self.trials)
        _require(len(paths) == len(set(paths)), "session trial paths must be unique")
        _require(
            all(trial.schema == "pipetune.trial/v1" for trial in self.trials),
            "session trials must be typed trial manifests",
        )
        if self.status == "running":
            _require(self.ended_at_utc is None, "running session must not have an end time")
            _require(self.failure_reason is None, "running session has failure reason")
        else:
            _require(self.ended_at_utc is not None, "finished session needs an end time")
            ended = _timestamp(self.ended_at_utc or "", "session.ended_at_utc")
            _require(ended >= started, "session timestamps are reversed")
            if self.status == "complete":
                _require(bool(self.trials), "complete session requires a trial")
                _require(
                    self.failure_reason is None,
                    "complete session has failure reason",
                )
            else:
                _require_string(self.failure_reason, "session.failure_reason")


@dataclasses.dataclass(frozen=True)
class TrialResult:
    session: ArtifactRef
    manifest: ArtifactRef
    success: bool
    target_metrics: ArtifactRef | None
    peer_metrics: ArtifactRef | None
    host_metrics: ArtifactRef | None
    failure_reason: str | None

    def __post_init__(self) -> None:
        _require(type(self.success) is bool, "trial result success must be a boolean")
        _require(
            self.session.schema == "pipetune.session/v1",
            "trial result requires a typed session manifest",
        )
        _require(
            self.manifest.schema == "pipetune.trial/v1",
            "trial result requires a typed trial manifest",
        )
        if self.success:
            _require(self.failure_reason is None, "successful result has failure reason")
            _require(
                self.target_metrics is not None and self.peer_metrics is not None,
                "successful result requires both endpoint metrics",
            )
            _require(
                self.target_metrics.schema == "axio.metrics/v1"
                and self.peer_metrics.schema == "axio.metrics/v1",
                "successful result metrics use an unsupported schema",
            )
            _require(
                self.host_metrics is not None
                and self.host_metrics.schema == "pipetune.host-metrics/v1",
                "successful result requires typed host metrics",
            )
        else:
            _require_string(self.failure_reason, "trial result failure_reason")
