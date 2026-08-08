"""Controller-neutral orchestration for one dual-endpoint Axio trial."""

from __future__ import annotations

import concurrent.futures
import copy
import dataclasses
import datetime as dt
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import time
import uuid
from collections.abc import Callable
from typing import Any, Protocol

from pipetune.artifacts import (
    artifact_ref,
    load_metric_sample,
    load_trial_manifest,
    write_metric_sample,
    write_trial_manifest,
)
from pipetune.metrics import AXIO_METRICS_SCHEMA, load_axio_jsonl
from pipetune.model import (
    ContractError,
    FingerprintSet,
    MetricSample,
    ProcessResult,
    ProviderStatus,
    TrialEndpoint,
    TrialManifest,
    TrialResult,
)
from pipetune.providers import ProviderResult, unavailable_counter
from pipetune.providers.pcm_pcie import PcmPcieProvider
from pipetune.providers.perf import PerfProvider
from pipetune.remote import (
    CommandOutcome,
    EndpointTransport,
    ResolvedEndpoint,
    TransportError,
    resolve_endpoint,
    transport_for,
)


TRIAL_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")


class MeasureError(RuntimeError):
    """Raised when a trial cannot be safely completed and published."""


@dataclasses.dataclass(frozen=True)
class MeasureRequest:
    target_config: pathlib.Path
    peer_config: pathlib.Path
    output: pathlib.Path
    configure_binary: pathlib.Path
    target_binary: str | None = None
    peer_binary: str | None = None
    perf_path: str = "/usr/bin/perf"
    pcm_pcie_path: str = "/usr/sbin/pcm-pcie"
    ready_timeout_seconds: float = 10.0
    completion_grace_seconds: float = 30.0

    def __post_init__(self) -> None:
        for value, name in (
            (self.target_config, "target config"),
            (self.peer_config, "peer config"),
            (self.output, "output"),
            (self.configure_binary, "axio-configure"),
        ):
            if not isinstance(value, pathlib.Path):
                raise MeasureError(f"{name} must be a pathlib.Path")
        for value, name in (
            (self.ready_timeout_seconds, "ready timeout"),
            (self.completion_grace_seconds, "completion grace"),
        ):
            if type(value) not in (int, float) or value <= 0:
                raise MeasureError(f"{name} must be positive")


class ConfigTool(Protocol):
    def resolve(
        self,
        *,
        endpoint_id: str,
        config_path: pathlib.Path,
        binary_override: str | None,
    ) -> ResolvedEndpoint: ...

    def dump(self, path: pathlib.Path) -> dict[str, object]: ...

    def materialize_runner_pair(
        self,
        *,
        target_input: pathlib.Path,
        peer_input: pathlib.Path,
        target_output: pathlib.Path,
        peer_output: pathlib.Path,
        target_metrics_path: str,
        peer_metrics_path: str,
    ) -> None: ...

    def fingerprints(self, path: pathlib.Path) -> dict[str, str]: ...


class AxioConfigTool:
    def __init__(self, binary: pathlib.Path) -> None:
        self.binary = binary

    def _run(self, *arguments: object) -> subprocess.CompletedProcess[str]:
        try:
            completed = subprocess.run(
                [str(self.binary), *(str(argument) for argument in arguments)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
        except OSError as error:
            raise MeasureError(f"cannot execute {self.binary}: {error}") from error
        if completed.returncode != 0:
            reason = completed.stderr.strip() or "axio-configure failed"
            raise MeasureError(reason)
        return completed

    def _json(self, *arguments: object) -> dict[str, Any]:
        completed = self._run(*arguments)
        try:
            document = json.loads(completed.stdout)
        except json.JSONDecodeError as error:
            raise MeasureError("axio-configure returned invalid JSON") from error
        if not isinstance(document, dict):
            raise MeasureError("axio-configure JSON must be an object")
        return document

    def resolve(
        self,
        *,
        endpoint_id: str,
        config_path: pathlib.Path,
        binary_override: str | None,
    ) -> ResolvedEndpoint:
        return resolve_endpoint(
            endpoint_id=endpoint_id,
            config_path=config_path,
            configure_binary=self.binary,
            binary_override=binary_override,
        )

    def dump(self, path: pathlib.Path) -> dict[str, object]:
        return self._json("dump", path)

    def fingerprints(self, path: pathlib.Path) -> dict[str, str]:
        document = self._json("fingerprints", path)
        if set(document) != {"build", "datapath", "deployment"} or not all(
            isinstance(value, str) and value for value in document.values()
        ):
            raise MeasureError("axio-configure returned invalid fingerprints")
        return {key: document[key] for key in ("build", "datapath", "deployment")}

    def materialize_runner_pair(
        self,
        *,
        target_input: pathlib.Path,
        peer_input: pathlib.Path,
        target_output: pathlib.Path,
        peer_output: pathlib.Path,
        target_metrics_path: str,
        peer_metrics_path: str,
    ) -> None:
        target_output.parent.mkdir(parents=True, exist_ok=True)
        for source, output, metrics_path in (
            (target_input, target_output, target_metrics_path),
            (peer_input, peer_output, peer_metrics_path),
        ):
            overrides = json.dumps(
                {
                    "metrics.human_output": False,
                    "metrics.jsonl_path": metrics_path,
                },
                allow_nan=False,
                separators=(",", ":"),
                sort_keys=True,
            )
            self._run("materialize", source, output, "--set-json", overrides)
            self.assert_runner_only_changes(self.dump(source), self.dump(output))
        self._run("validate-pair", target_output, peer_output)

    @staticmethod
    def assert_runner_only_changes(
        source: dict[str, object], materialized: dict[str, object]
    ) -> None:
        expected = copy.deepcopy(source)
        source_metrics = expected.get("metrics")
        output_metrics = materialized.get("metrics")
        if not isinstance(source_metrics, dict) or not isinstance(output_metrics, dict):
            raise MeasureError("configuration is missing metrics")
        if source_metrics.get("enabled") is not True:
            raise MeasureError("PipeTune measure requires metrics.enabled = true")
        source_metrics["jsonl_path"] = output_metrics.get("jsonl_path")
        source_metrics["human_output"] = False
        if materialized != expected:
            raise MeasureError("runner changed a non-artifact config field")


@dataclasses.dataclass
class _EndpointRun:
    resolved: ResolvedEndpoint
    source_config: pathlib.Path
    materialized_config: pathlib.Path
    transport: EndpointTransport
    remote_root: str
    trials_root: str
    remote_config: str
    remote_metrics: str
    state_path: str
    stdout_path: str
    stderr_path: str
    fingerprints: FingerprintSet
    handle: object | None = None
    outcome: CommandOutcome | None = None


@dataclasses.dataclass(frozen=True)
class _ProviderSetup:
    perf_provider: PerfProvider | None
    perf_result: ProviderResult | None
    pcm_provider: PcmPcieProvider | None
    pcm_result: ProviderResult | None
    raw_payloads: dict[str, bytes]


def _utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _write_bytes_atomic(path: pathlib.Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = pathlib.Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        if hasattr(os, "O_DIRECTORY"):
            directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    except BaseException:
        try:
            os.close(descriptor)
        except OSError:
            pass
        temporary.unlink(missing_ok=True)
        raise


def _remote_layout(spec: EndpointSpec, trial_id: str) -> tuple[str, str]:
    trials = pathlib.PurePosixPath(spec.workdir) / ".pipetune" / "trials"
    root = trials / trial_id / spec.endpoint_id
    return str(trials), str(root)


def _run_capture(
    endpoint: _EndpointRun,
    *,
    label: str,
    argv: tuple[str, ...],
    timeout_seconds: float,
    use_sudo: bool = False,
) -> tuple[CommandOutcome, bytes, bytes]:
    prefix = pathlib.PurePosixPath(endpoint.remote_root) / "control" / label
    stdout_path = str(prefix.with_suffix(".stdout"))
    stderr_path = str(prefix.with_suffix(".stderr"))
    try:
        outcome = endpoint.transport.run(
            session_id=f"{endpoint.resolved.spec.endpoint_id}-{label}",
            argv=argv,
            cwd=endpoint.resolved.spec.workdir,
            state_path=str(prefix.with_suffix(".state.json")),
            stdout_path=stdout_path,
            stderr_path=stderr_path,
            timeout_seconds=timeout_seconds,
            use_sudo=use_sudo,
        )
        stdout = endpoint.transport.get_bytes(stdout_path)
        stderr = endpoint.transport.get_bytes(stderr_path)
    except (OSError, TransportError) as error:
        raise MeasureError(f"{label} transport failed: {error}") from error
    return outcome, stdout, stderr


def _require_command(outcome: CommandOutcome, stdout: bytes, label: str) -> str:
    if outcome.status != "exited" or outcome.return_code != 0:
        raise MeasureError(f"{label} failed")
    try:
        return stdout.decode("utf-8", errors="strict").strip()
    except UnicodeDecodeError as error:
        raise MeasureError(f"{label} returned invalid UTF-8") from error


def _probe_fingerprints(
    endpoint: _EndpointRun,
    config_tool: ConfigTool,
) -> FingerprintSet:
    git_outcome, git_stdout, _ = _run_capture(
        endpoint,
        label="git-commit",
        argv=("git", "-C", endpoint.resolved.spec.workdir, "rev-parse", "HEAD"),
        timeout_seconds=10.0,
    )
    git_commit = _require_command(git_outcome, git_stdout, "git commit probe")
    binary_outcome, binary_stdout, _ = _run_capture(
        endpoint,
        label="binary-sha256",
        argv=("sha256sum", endpoint.resolved.binary_path),
        timeout_seconds=30.0,
    )
    binary_line = _require_command(
        binary_outcome, binary_stdout, "binary SHA-256 probe"
    )
    binary_sha = binary_line.split(maxsplit=1)[0]
    values = config_tool.fingerprints(endpoint.materialized_config)
    return FingerprintSet(
        git_commit=git_commit,
        binary_sha256=binary_sha,
        source_config_sha256=_sha256(endpoint.source_config),
        build=values["build"],
        datapath=values["datapath"],
        deployment=values["deployment"],
    )


def _provider_failure(
    name: str, path: str, reason: str, counter_names: tuple[str, ...]
) -> ProviderResult:
    return ProviderResult(
        ProviderStatus(
            name=name,
            available=False,
            path=path,
            version=None,
            reason=reason,
        ),
        tuple(unavailable_counter(counter, reason) for counter in counter_names),
    )


def _collect_perf(
    endpoint: _EndpointRun,
    provider: PerfProvider,
    *,
    pid: int,
    sample_interval_seconds: float,
) -> tuple[ProviderResult, dict[str, bytes]]:
    pair_interval = sample_interval_seconds / 2.0
    raw: dict[str, bytes] = {}
    stderr_parts: list[bytes] = []
    outcomes: list[CommandOutcome] = []
    for label, command in zip(
        ("perf-load", "perf-store"),
        provider.commands(pid=pid, sample_interval_seconds=pair_interval),
        strict=True,
    ):
        outcome, stdout, stderr = _run_capture(
            endpoint,
            label=label,
            argv=command,
            timeout_seconds=pair_interval + 10.0,
            use_sudo=endpoint.resolved.spec.use_sudo,
        )
        outcomes.append(outcome)
        raw[f"{label}.stdout"] = stdout
        raw[f"{label}.stderr"] = stderr
        stderr_parts.append(stderr)
    combined = b"\n".join(stderr_parts)
    parsed = provider.parse(combined, sample_interval_seconds=pair_interval)
    if any(outcome.status != "exited" or outcome.return_code != 0 for outcome in outcomes):
        if parsed.status.available:
            return (
                _provider_failure(
                    "perf",
                    provider.path,
                    "perf collection command failed",
                    ("llc_load", "llc_store"),
                ),
                raw,
            )
    return parsed, raw


def _collect_pcm(
    endpoint: _EndpointRun,
    provider: PcmPcieProvider,
    *,
    sample_interval_seconds: float,
) -> tuple[ProviderResult, dict[str, bytes]]:
    remote_csv = str(pathlib.PurePosixPath(endpoint.remote_root) / "pcm-pcie.csv")
    outcome, stdout, stderr = _run_capture(
        endpoint,
        label="pcm-pcie",
        argv=provider.command(
            output_path=remote_csv,
            sample_interval_seconds=sample_interval_seconds,
        ),
        timeout_seconds=sample_interval_seconds + 10.0,
        use_sudo=endpoint.resolved.spec.use_sudo,
    )
    try:
        raw_csv = endpoint.transport.get_bytes(remote_csv)
    except TransportError as error:
        raise MeasureError(f"pcm-pcie artifact retrieval failed: {error}") from error
    raw = {
        "pcm-pcie.csv": raw_csv,
        "pcm-pcie.stdout": stdout,
        "pcm-pcie.stderr": stderr,
    }
    parsed = provider.parse(
        raw_csv,
        stderr=stderr,
        socket_id=endpoint.resolved.spec.numa_node,
    )
    if outcome.status != "exited" or outcome.return_code != 0:
        if parsed.status.available:
            return (
                _provider_failure(
                    "pcm_pcie",
                    provider.path,
                    "pcm-pcie collection command failed",
                    ("io_read", "io_write"),
                ),
                raw,
            )
    return parsed, raw


def _probe_providers(
    endpoint: _EndpointRun,
    *,
    perf_path: str,
    pcm_path: str,
) -> _ProviderSetup:
    raw_payloads: dict[str, bytes] = {}
    perf_probe, perf_probe_stdout, perf_probe_stderr = _run_capture(
        endpoint,
        label="perf-version",
        argv=PerfProvider.version_command(perf_path),
        timeout_seconds=10.0,
    )
    raw_payloads["perf-version.stdout"] = perf_probe_stdout
    raw_payloads["perf-version.stderr"] = perf_probe_stderr
    try:
        if perf_probe.status != "exited" or perf_probe.return_code != 0:
            raise ValueError("perf version probe failed")
        perf_provider: PerfProvider | None = PerfProvider(
            perf_path,
            PerfProvider.parse_version(perf_probe_stdout),
        )
        perf_result: ProviderResult | None = None
    except ValueError as error:
        perf_provider = None
        perf_result = _provider_failure(
            "perf", perf_path, str(error), ("llc_load", "llc_store")
        )

    pcm_probe, pcm_probe_stdout, pcm_probe_stderr = _run_capture(
        endpoint,
        label="pcm-version",
        argv=PcmPcieProvider.version_command(),
        timeout_seconds=10.0,
    )
    raw_payloads["pcm-version.stdout"] = pcm_probe_stdout
    raw_payloads["pcm-version.stderr"] = pcm_probe_stderr
    try:
        if pcm_probe.status != "exited" or pcm_probe.return_code != 0:
            raise ValueError("pcm version probe failed")
        pcm_provider: PcmPcieProvider | None = PcmPcieProvider(
            pcm_path,
            PcmPcieProvider.parse_version(pcm_probe_stdout),
        )
        pcm_result: ProviderResult | None = None
    except ValueError as error:
        pcm_provider = None
        pcm_result = _provider_failure(
            "pcm_pcie", pcm_path, str(error), ("io_read", "io_write")
        )
    return _ProviderSetup(
        perf_provider=perf_provider,
        perf_result=perf_result,
        pcm_provider=pcm_provider,
        pcm_result=pcm_result,
        raw_payloads=raw_payloads,
    )


def _collect_host_metrics(
    endpoint: _EndpointRun,
    *,
    setup: _ProviderSetup,
    stage_root: pathlib.Path,
    sample_interval_seconds: float,
) -> MetricSample:
    started = _utc_now()
    try:
        state = json.loads(endpoint.transport.get_bytes(endpoint.state_path))
        pid = state["workload_pid"]
    except (KeyError, json.JSONDecodeError, TransportError, TypeError) as error:
        raise MeasureError("target worker state does not expose a workload PID") from error
    if type(pid) is not int or pid <= 0:
        raise MeasureError("target worker workload PID is invalid")

    perf_result = setup.perf_result
    pcm_result = setup.pcm_result
    raw_payloads = dict(setup.raw_payloads)
    futures: dict[str, concurrent.futures.Future] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        if setup.perf_provider is not None:
            futures["perf"] = executor.submit(
                _collect_perf,
                endpoint,
                setup.perf_provider,
                pid=pid,
                sample_interval_seconds=sample_interval_seconds,
            )
        if setup.pcm_provider is not None:
            futures["pcm"] = executor.submit(
                _collect_pcm,
                endpoint,
                setup.pcm_provider,
                sample_interval_seconds=sample_interval_seconds,
            )
        for name, future in futures.items():
            result, payloads = future.result()
            raw_payloads.update(payloads)
            if name == "perf":
                perf_result = result
            else:
                pcm_result = result
    assert perf_result is not None and pcm_result is not None

    raw_references = []
    for name, payload in sorted(raw_payloads.items()):
        path = stage_root / "providers" / name
        _write_bytes_atomic(path, payload)
        raw_references.append(artifact_ref(stage_root, path))
    return MetricSample(
        schema="pipetune.host-metrics/v1",
        sample_id=f"{endpoint.resolved.spec.endpoint_id}-host-metrics",
        endpoint_id=endpoint.resolved.spec.endpoint_id,
        started_at_utc=started,
        ended_at_utc=_utc_now(),
        sample_interval_seconds=sample_interval_seconds,
        socket_id=endpoint.resolved.spec.numa_node,
        counters=(*perf_result.counters, *pcm_result.counters),
        providers=(perf_result.status, pcm_result.status),
        raw_artifacts=tuple(raw_references),
    )


def _trial_windows(document: dict[str, object]) -> tuple[int, int, float]:
    try:
        other = document["other"]
        tuning = document["tuning"]
        iterations = other["iterations"]
        window_seconds = other["window_seconds"]
        warmup = tuning["warmup_windows"]
        sample = tuning["sample_windows"]
    except (KeyError, TypeError) as error:
        raise MeasureError("configuration is missing trial window policy") from error
    if any(type(value) is not int for value in (iterations, window_seconds, warmup, sample)):
        raise MeasureError("trial window policy must contain integers")
    if window_seconds <= 0 or warmup < 0 or sample <= 0:
        raise MeasureError("trial window policy is invalid")
    if iterations < warmup + sample:
        raise MeasureError("other.iterations is shorter than warmup plus sample windows")
    return warmup, sample, float(window_seconds)


def _poll_window_ids(
    endpoint: _EndpointRun,
    *,
    scratch_path: pathlib.Path,
    previous: tuple[int, ...],
) -> tuple[int, ...] | None:
    try:
        payload = endpoint.transport.get_bytes(endpoint.remote_metrics)
    except TransportError:
        return None
    if not payload or not payload.endswith(b"\n"):
        return None
    _write_bytes_atomic(scratch_path, payload)
    try:
        windows = load_axio_jsonl(scratch_path, schema=AXIO_METRICS_SCHEMA)
    except (ContractError, OSError) as error:
        raise MeasureError(
            f"{endpoint.resolved.spec.endpoint_id} warmup metrics are invalid: {error}"
        ) from error
    current = tuple(window.window_id for window in windows)
    if current[: len(previous)] != previous:
        raise MeasureError(
            f"{endpoint.resolved.spec.endpoint_id} warmup metrics regressed"
        )
    return current


def _wait_for_warmup(
    endpoints: list[_EndpointRun],
    *,
    warmup_windows: int,
    timeout_seconds: float,
    scratch_root: pathlib.Path,
    sleeper: Callable[[float], None],
) -> None:
    if warmup_windows == 0:
        return
    deadline = time.monotonic() + timeout_seconds
    observed = {
        endpoint.resolved.spec.endpoint_id: tuple() for endpoint in endpoints
    }
    scratch_root.mkdir(parents=True, exist_ok=True)
    try:
        while time.monotonic() < deadline:
            for endpoint in endpoints:
                endpoint_id = endpoint.resolved.spec.endpoint_id
                if len(observed[endpoint_id]) >= warmup_windows:
                    continue
                if endpoint.handle is None or not endpoint.transport.is_running(
                    endpoint.handle
                ):
                    raise MeasureError(
                        f"{endpoint_id} endpoint exited before warmup completed"
                    )
                current = _poll_window_ids(
                    endpoint,
                    scratch_path=scratch_root / f"{endpoint_id}.jsonl",
                    previous=observed[endpoint_id],
                )
                if current is not None:
                    observed[endpoint_id] = current
            if all(len(windows) >= warmup_windows for windows in observed.values()):
                return
            sleeper(0.05)
    finally:
        shutil.rmtree(scratch_root, ignore_errors=True)
    missing = ", ".join(
        f"{endpoint_id}={len(windows)}/{warmup_windows}"
        for endpoint_id, windows in sorted(observed.items())
        if len(windows) < warmup_windows
    )
    raise MeasureError(f"warmup metrics timed out ({missing})")


def _endpoint_artifacts(
    endpoint: _EndpointRun,
    *,
    stage_root: pathlib.Path,
    warmup_windows: int,
    sample_windows: int,
) -> TrialEndpoint:
    if endpoint.outcome is None:
        raise MeasureError("endpoint has no process outcome")
    outcome = endpoint.outcome
    if outcome.status != "exited" or outcome.return_code != 0:
        raise MeasureError(
            f"{endpoint.resolved.spec.endpoint_id} endpoint did not exit successfully"
        )
    local_dir = stage_root / "endpoints" / endpoint.resolved.spec.endpoint_id
    try:
        stdout = endpoint.transport.get_bytes(endpoint.stdout_path)
        stderr = endpoint.transport.get_bytes(endpoint.stderr_path)
        metrics = endpoint.transport.get_bytes(endpoint.remote_metrics)
    except TransportError as error:
        raise MeasureError(f"endpoint artifact retrieval failed: {error}") from error
    stdout_path = local_dir / "stdout.log"
    stderr_path = local_dir / "stderr.log"
    metrics_path = local_dir / "metrics.jsonl"
    _write_bytes_atomic(stdout_path, stdout)
    _write_bytes_atomic(stderr_path, stderr)
    _write_bytes_atomic(metrics_path, metrics)
    windows = load_axio_jsonl(metrics_path, schema=AXIO_METRICS_SCHEMA)
    if len(windows) < warmup_windows + sample_windows:
        raise MeasureError("endpoint metrics contain too few windows")
    return TrialEndpoint(
        spec=endpoint.resolved.spec,
        fingerprints=endpoint.fingerprints,
        process=ProcessResult(
            argv=outcome.argv,
            status=outcome.status,
            return_code=outcome.return_code,
            failure_reason=outcome.failure_reason,
            started_at_utc=outcome.started_at_utc,
            ended_at_utc=outcome.ended_at_utc,
            stdout=artifact_ref(stage_root, stdout_path),
            stderr=artifact_ref(stage_root, stderr_path),
        ),
        artifacts=(
            artifact_ref(stage_root, metrics_path, schema=AXIO_METRICS_SCHEMA),
            artifact_ref(stage_root, endpoint.source_config),
            artifact_ref(stage_root, endpoint.materialized_config),
        ),
    )


def _cleanup(endpoints: list[_EndpointRun]) -> list[str]:
    failures = []
    for endpoint in endpoints:
        if endpoint.handle is not None and endpoint.outcome is None:
            try:
                endpoint.outcome = endpoint.transport.terminate(endpoint.handle)
            except (OSError, TransportError) as error:
                failures.append(f"{endpoint.resolved.spec.endpoint_id} terminate: {error}")
    for endpoint in endpoints:
        try:
            endpoint.transport.remove_tree(
                endpoint.remote_root,
                containment_root=endpoint.trials_root,
            )
        except (OSError, TransportError) as error:
            failures.append(f"{endpoint.resolved.spec.endpoint_id} cleanup: {error}")
    return failures


def measure(
    request: MeasureRequest,
    *,
    config_tool: ConfigTool | None = None,
    transport_factory: Callable[[EndpointSpec], EndpointTransport] = transport_for,
    sleeper: Callable[[float], None] = time.sleep,
    trial_id_factory: Callable[[], str] = lambda: f"trial-{uuid.uuid4().hex}",
) -> TrialResult:
    tool = config_tool or AxioConfigTool(request.configure_binary)
    trial_id = trial_id_factory()
    if not TRIAL_ID_PATTERN.fullmatch(trial_id):
        raise MeasureError("trial ID is not path-safe")
    if request.output.exists():
        raise MeasureError(f"output already exists: {request.output}")
    request.output.parent.mkdir(parents=True, exist_ok=True)
    stage_root = request.output.parent / f".{request.output.name}.{trial_id}.tmp"
    if stage_root.exists():
        raise MeasureError(f"trial staging path already exists: {stage_root}")
    stage_root.mkdir()
    started_at = _utc_now()
    endpoints: list[_EndpointRun] = []
    try:
        source_target = tool.resolve(
            endpoint_id="target",
            config_path=request.target_config,
            binary_override=request.target_binary,
        )
        source_peer = tool.resolve(
            endpoint_id="peer",
            config_path=request.peer_config,
            binary_override=request.peer_binary,
        )
        if {source_target.spec.role, source_peer.spec.role} != {"client", "server"}:
            raise MeasureError("target and peer must contain one client and one server")
        if source_target.spec.backend != source_peer.spec.backend:
            raise MeasureError("target and peer backends must match")
        target_document = tool.dump(request.target_config)
        peer_document = tool.dump(request.peer_config)
        warmup, sample, window_seconds = _trial_windows(target_document)
        peer_warmup, peer_sample, peer_window_seconds = _trial_windows(peer_document)
        if (warmup, sample, window_seconds) != (
            peer_warmup,
            peer_sample,
            peer_window_seconds,
        ):
            raise MeasureError("target and peer trial window policies must match")

        source_dir = stage_root / "configs" / "source"
        materialized_dir = stage_root / "configs" / "materialized"
        source_dir.mkdir(parents=True)
        materialized_dir.mkdir(parents=True)
        source_paths = {
            "target": source_dir / "target.toml",
            "peer": source_dir / "peer.toml",
        }
        shutil.copyfile(request.target_config, source_paths["target"])
        shutil.copyfile(request.peer_config, source_paths["peer"])
        materialized_paths = {
            "target": materialized_dir / "target.toml",
            "peer": materialized_dir / "peer.toml",
        }
        target_trials, target_remote_root = _remote_layout(source_target.spec, trial_id)
        peer_trials, peer_remote_root = _remote_layout(source_peer.spec, trial_id)
        remote_roots = {"target": target_remote_root, "peer": peer_remote_root}
        remote_metrics = {
            endpoint_id: str(pathlib.PurePosixPath(root) / "metrics.jsonl")
            for endpoint_id, root in remote_roots.items()
        }
        tool.materialize_runner_pair(
            target_input=request.target_config,
            peer_input=request.peer_config,
            target_output=materialized_paths["target"],
            peer_output=materialized_paths["peer"],
            target_metrics_path=remote_metrics["target"],
            peer_metrics_path=remote_metrics["peer"],
        )
        resolved = {
            "target": tool.resolve(
                endpoint_id="target",
                config_path=materialized_paths["target"],
                binary_override=request.target_binary,
            ),
            "peer": tool.resolve(
                endpoint_id="peer",
                config_path=materialized_paths["peer"],
                binary_override=request.peer_binary,
            ),
        }
        for endpoint_id, trials_root, remote_root in (
            ("target", target_trials, target_remote_root),
            ("peer", peer_trials, peer_remote_root),
        ):
            item = resolved[endpoint_id]
            transport = transport_factory(item.spec)
            remote_config = str(pathlib.PurePosixPath(remote_root) / "config.toml")
            endpoint = _EndpointRun(
                resolved=item,
                source_config=source_paths[endpoint_id],
                materialized_config=materialized_paths[endpoint_id],
                transport=transport,
                remote_root=remote_root,
                trials_root=trials_root,
                remote_config=remote_config,
                remote_metrics=remote_metrics[endpoint_id],
                state_path=str(pathlib.PurePosixPath(remote_root) / "axio.state.json"),
                stdout_path=str(pathlib.PurePosixPath(remote_root) / "axio.stdout"),
                stderr_path=str(pathlib.PurePosixPath(remote_root) / "axio.stderr"),
                fingerprints=FingerprintSet(
                    git_commit="0" * 40,
                    binary_sha256="0" * 64,
                    source_config_sha256="0" * 64,
                    build="pending",
                    datapath="pending",
                    deployment="pending",
                ),
            )
            endpoints.append(endpoint)
            transport.put_bytes(remote_config, materialized_paths[endpoint_id].read_bytes())
        for endpoint in endpoints:
            endpoint.fingerprints = _probe_fingerprints(endpoint, tool)
        target = next(
            endpoint
            for endpoint in endpoints
            if endpoint.resolved.spec.endpoint_id == "target"
        )
        provider_setup = _probe_providers(
            target,
            perf_path=request.perf_path,
            pcm_path=request.pcm_pcie_path,
        )

        completion_timeout = (
            (warmup + sample) * window_seconds + request.completion_grace_seconds
        )
        for endpoint in sorted(
            endpoints,
            key=lambda item: 0 if item.resolved.spec.role == "server" else 1,
        ):
            endpoint.handle = endpoint.transport.start(
                session_id=f"{trial_id}-{endpoint.resolved.spec.endpoint_id}",
                argv=(
                    endpoint.resolved.binary_path,
                    "--config",
                    endpoint.remote_config,
                ),
                cwd=endpoint.resolved.spec.workdir,
                state_path=endpoint.state_path,
                stdout_path=endpoint.stdout_path,
                stderr_path=endpoint.stderr_path,
                use_sudo=endpoint.resolved.spec.use_sudo,
                ready_timeout_seconds=request.ready_timeout_seconds,
            )
        _wait_for_warmup(
            endpoints,
            warmup_windows=warmup,
            timeout_seconds=request.ready_timeout_seconds + warmup * window_seconds,
            scratch_root=stage_root / ".warmup",
            sleeper=sleeper,
        )
        host_metrics = _collect_host_metrics(
            target,
            setup=provider_setup,
            stage_root=stage_root,
            sample_interval_seconds=sample * window_seconds,
        )
        host_metrics_path = stage_root / "host-metrics.json"
        write_metric_sample(host_metrics_path, host_metrics)
        load_metric_sample(host_metrics_path, artifact_root=stage_root)

        for endpoint in sorted(
            endpoints,
            key=lambda item: 0 if item.resolved.spec.role == "client" else 1,
        ):
            endpoint.outcome = endpoint.transport.wait(
                endpoint.handle,
                timeout_seconds=completion_timeout,
            )
            if endpoint.outcome.status != "exited" or endpoint.outcome.return_code != 0:
                raise MeasureError(
                    f"{endpoint.resolved.spec.endpoint_id} endpoint failed"
                )
        endpoint_artifacts = tuple(
            sorted(
                (
                    _endpoint_artifacts(
                        endpoint,
                        stage_root=stage_root,
                        warmup_windows=warmup,
                        sample_windows=sample,
                    )
                    for endpoint in endpoints
                ),
                key=lambda item: 0 if item.spec.role == "client" else 1,
            )
        )
        cleanup_failures = _cleanup(endpoints)
        if cleanup_failures:
            raise MeasureError("; ".join(cleanup_failures))
        manifest = TrialManifest(
            schema="pipetune.trial/v1",
            trial_id=trial_id,
            target_endpoint_id="target",
            started_at_utc=started_at,
            ended_at_utc=_utc_now(),
            status="success",
            cleanup_status="clean",
            endpoints=endpoint_artifacts,
            host_metrics=artifact_ref(
                stage_root,
                host_metrics_path,
                schema="pipetune.host-metrics/v1",
            ),
            failure_reason=None,
        )
        manifest_path = stage_root / "trial.json"
        write_trial_manifest(manifest_path, manifest)
        load_trial_manifest(manifest_path, artifact_root=stage_root)
        os.replace(stage_root, request.output)
        if hasattr(os, "O_DIRECTORY"):
            directory = os.open(
                request.output.parent,
                os.O_RDONLY | os.O_DIRECTORY,
            )
            try:
                os.fsync(directory)
            finally:
                os.close(directory)

        published_manifest = request.output / "trial.json"
        published = load_trial_manifest(
            published_manifest,
            artifact_root=request.output,
        )
        by_id = {
            endpoint.spec.endpoint_id: next(
                artifact
                for artifact in endpoint.artifacts
                if artifact.schema == AXIO_METRICS_SCHEMA
            )
            for endpoint in published.endpoints
        }
        return TrialResult(
            manifest=artifact_ref(
                request.output,
                published_manifest,
                schema="pipetune.trial/v1",
            ),
            success=True,
            target_metrics=by_id["target"],
            peer_metrics=by_id["peer"],
            host_metrics=published.host_metrics,
            failure_reason=None,
        )
    except BaseException as error:
        cleanup_failures = _cleanup(endpoints)
        shutil.rmtree(stage_root, ignore_errors=True)
        if isinstance(error, (KeyboardInterrupt, SystemExit)):
            raise
        reason = str(error)
        if cleanup_failures:
            reason = f"{reason}; cleanup: {'; '.join(cleanup_failures)}"
        raise MeasureError(reason) from error


__all__ = [
    "AxioConfigTool",
    "MeasureError",
    "MeasureRequest",
    "measure",
]
