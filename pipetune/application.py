"""Production bootstrap/resume/status workflow for the script-based tuner."""

from __future__ import annotations

import dataclasses
import hashlib
import json
import os
import pathlib
import re
import shutil
import tempfile
import uuid
from collections.abc import Callable
from typing import Any

from pipetune.artifacts import artifact_ref
from pipetune.candidates import generate_lock_averse_candidates
from pipetune.controller import ColdStartController, MeasureTrialExecutor, TuningLoop
from pipetune.model import FingerprintSet, PROVIDER_NAMES
from pipetune.objective import ObjectivePolicy
from pipetune.remote import EndpointTransport, ResolvedEndpoint, transport_for
from pipetune.reporting import publish_session_outputs, status_document
from pipetune.runner import AxioConfigTool, MeasureRequest
from pipetune.session import (
    ConfigPair,
    EndpointIdentity,
    SessionIdentity,
    SessionState,
    TuningSessionStore,
)


class ApplicationError(RuntimeError):
    """Raised when a tuner session cannot be initialized or resumed safely."""


@dataclasses.dataclass(frozen=True)
class BootstrapRequest:
    target_config: pathlib.Path
    peer_config: pathlib.Path
    output: pathlib.Path
    configure_binary: pathlib.Path
    max_iterations: int
    target_binary: str | None = None
    peer_binary: str | None = None

    def __post_init__(self) -> None:
        if type(self.max_iterations) is not int or self.max_iterations < 1:
            raise ApplicationError("max iterations must be a positive integer")
        for value, label in (
            (self.target_config, "target config"),
            (self.peer_config, "peer config"),
            (self.output, "output"),
            (self.configure_binary, "axio-configure"),
        ):
            if not isinstance(value, pathlib.Path):
                raise ApplicationError(f"{label} must be a pathlib.Path")


@dataclasses.dataclass(frozen=True)
class ResumeRequest:
    session: pathlib.Path
    configure_binary: pathlib.Path

    def __post_init__(self) -> None:
        if not isinstance(self.session, pathlib.Path) or not isinstance(
            self.configure_binary, pathlib.Path
        ):
            raise ApplicationError("resume paths must be pathlib.Path values")


@dataclasses.dataclass(frozen=True)
class ApplicationResult:
    document: dict[str, object]


@dataclasses.dataclass(frozen=True)
class _Settings:
    max_iterations: int
    infrastructure_failure_limit: int
    policy: ObjectivePolicy
    target_binary: str | None
    peer_binary: str | None
    configure_sha256: str


def _object(value: object, location: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ApplicationError(f"{location} must be an object")
    return value


def _integer(value: object, location: str, *, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise ApplicationError(f"{location} must be an integer >= {minimum}")
    return value


def _number(value: object, location: str) -> float:
    if type(value) not in (int, float):
        raise ApplicationError(f"{location} must be a number")
    return float(value)


def _settings_from_config(
    document: dict[str, object],
    *,
    max_iterations: int,
    target_binary: str | None,
    peer_binary: str | None,
    configure_sha256: str,
) -> _Settings:
    tuning = _object(document.get("tuning"), "tuning")
    noise = _object(tuning.get("noise"), "tuning.noise")
    configured_max = _integer(
        tuning.get("max_iterations"), "tuning.max_iterations", minimum=1
    )
    if max_iterations > configured_max:
        raise ApplicationError(
            "--max-iterations exceeds tuning.max_iterations in the target config"
        )
    return _Settings(
        max_iterations=max_iterations,
        infrastructure_failure_limit=_integer(
            tuning.get("infrastructure_failure_limit"),
            "tuning.infrastructure_failure_limit",
            minimum=1,
        ),
        policy=ObjectivePolicy(
            latency_slo_us=_number(
                tuning.get("latency_slo_us"), "tuning.latency_slo_us"
            ),
            latency_relative_floor=_number(
                noise.get("latency_relative_floor"),
                "tuning.noise.latency_relative_floor",
            ),
            throughput_relative_floor=_number(
                noise.get("throughput_relative_floor"),
                "tuning.noise.throughput_relative_floor",
            ),
        ),
        target_binary=target_binary,
        peer_binary=peer_binary,
        configure_sha256=configure_sha256,
    )


def _settings_document(settings: _Settings) -> dict[str, object]:
    return {
        "infrastructure_failure_limit": settings.infrastructure_failure_limit,
        "configure_sha256": settings.configure_sha256,
        "max_iterations": settings.max_iterations,
        "objective": {
            "latency_relative_floor": settings.policy.latency_relative_floor,
            "latency_slo_us": settings.policy.latency_slo_us,
            "throughput_relative_floor": settings.policy.throughput_relative_floor,
        },
        "peer_binary": settings.peer_binary,
        "schema": "pipetune.bootstrap/v1",
        "target_binary": settings.target_binary,
    }


def _settings_from_state(value: object) -> _Settings:
    document = _object(value, "state.details.bootstrap")
    if document.get("schema") != "pipetune.bootstrap/v1":
        raise ApplicationError("bootstrap settings schema is unsupported")
    objective = _object(document.get("objective"), "bootstrap.objective")
    target_binary = document.get("target_binary")
    peer_binary = document.get("peer_binary")
    configure_sha256 = document.get("configure_sha256")
    if target_binary is not None and not isinstance(target_binary, str):
        raise ApplicationError("bootstrap target binary is invalid")
    if peer_binary is not None and not isinstance(peer_binary, str):
        raise ApplicationError("bootstrap peer binary is invalid")
    if not isinstance(configure_sha256, str) or re.fullmatch(
        r"[0-9a-f]{64}", configure_sha256
    ) is None:
        raise ApplicationError("bootstrap axio-configure SHA-256 is invalid")
    return _Settings(
        max_iterations=_integer(
            document.get("max_iterations"), "bootstrap.max_iterations", minimum=1
        ),
        infrastructure_failure_limit=_integer(
            document.get("infrastructure_failure_limit"),
            "bootstrap.infrastructure_failure_limit",
            minimum=1,
        ),
        policy=ObjectivePolicy(
            latency_slo_us=_number(
                objective.get("latency_slo_us"), "bootstrap.objective.latency_slo_us"
            ),
            latency_relative_floor=_number(
                objective.get("latency_relative_floor"),
                "bootstrap.objective.latency_relative_floor",
            ),
            throughput_relative_floor=_number(
                objective.get("throughput_relative_floor"),
                "bootstrap.objective.throughput_relative_floor",
            ),
        ),
        target_binary=target_binary,
        peer_binary=peer_binary,
        configure_sha256=configure_sha256,
    )


def _canonical_sha(document: dict[str, object]) -> str:
    payload = (
        json.dumps(
            document,
            allow_nan=False,
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        )
        + "\n"
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _capture(
    transport: EndpointTransport,
    *,
    endpoint: ResolvedEndpoint,
    remote_root: pathlib.PurePosixPath,
    label: str,
    argv: tuple[str, ...],
    timeout_seconds: float,
) -> str:
    prefix = remote_root / label
    outcome = transport.run(
        session_id=f"bootstrap-{endpoint.spec.endpoint_id}-{label}",
        argv=argv,
        cwd=endpoint.spec.workdir,
        state_path=str(prefix.with_suffix(".state.json")),
        stdout_path=str(prefix.with_suffix(".stdout")),
        stderr_path=str(prefix.with_suffix(".stderr")),
        timeout_seconds=timeout_seconds,
    )
    if outcome.status != "exited" or outcome.return_code != 0:
        raise ApplicationError(f"{endpoint.spec.endpoint_id} {label} probe failed")
    try:
        return transport.get_bytes(str(prefix.with_suffix(".stdout"))).decode(
            "utf-8", errors="strict"
        ).strip()
    except UnicodeError as error:
        raise ApplicationError(
            f"{endpoint.spec.endpoint_id} {label} probe returned invalid UTF-8"
        ) from error


def _probe_endpoint(
    *,
    session_id: str,
    endpoint: ResolvedEndpoint,
    source_config: pathlib.Path,
    config_tool: AxioConfigTool,
    transport_factory: Callable[..., EndpointTransport],
) -> FingerprintSet:
    transport = transport_factory(endpoint.spec)
    containment = (
        pathlib.PurePosixPath(endpoint.spec.workdir) / ".pipetune" / "bootstrap"
    )
    remote_root = containment / session_id / endpoint.spec.endpoint_id
    try:
        git_commit = _capture(
            transport,
            endpoint=endpoint,
            remote_root=remote_root,
            label="git-commit",
            argv=("git", "-C", endpoint.spec.workdir, "rev-parse", "HEAD"),
            timeout_seconds=10.0,
        )
        binary_line = _capture(
            transport,
            endpoint=endpoint,
            remote_root=remote_root,
            label="binary-sha256",
            argv=("sha256sum", endpoint.binary_path),
            timeout_seconds=30.0,
        )
    finally:
        transport.remove_tree(str(remote_root), containment_root=str(containment))
    binary_sha = binary_line.split(maxsplit=1)[0]
    values = config_tool.fingerprints(source_config)
    return FingerprintSet(
        git_commit=git_commit,
        binary_sha256=binary_sha,
        source_config_sha256=hashlib.sha256(source_config.read_bytes()).hexdigest(),
        build=values["build"],
        datapath=values["datapath"],
        deployment=values["deployment"],
    )


def _endpoint_identity(
    *,
    root: pathlib.Path,
    source: pathlib.Path,
    resolved: ResolvedEndpoint,
    document: dict[str, object],
    fingerprints: FingerprintSet,
) -> EndpointIdentity:
    return EndpointIdentity(
        endpoint_id=resolved.spec.endpoint_id,
        role=resolved.spec.role,
        source_config=artifact_ref(root, source),
        fingerprints=fingerprints,
        canonical_config_sha256=_canonical_sha(document),
    )


def _initialize(
    request: BootstrapRequest,
    *,
    config_tool: AxioConfigTool,
    transport_factory: Callable[..., EndpointTransport],
    session_id: str,
) -> pathlib.Path:
    output = request.output.resolve()
    if output.exists():
        raise ApplicationError(f"output already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = pathlib.Path(
        tempfile.mkdtemp(
            prefix=f".{output.name}.bootstrap.",
            dir=output.parent,
        )
    )
    try:
        inputs = stage / "inputs"
        accepted_dir = stage / "configs" / "accepted-0000"
        inputs.mkdir(parents=True)
        accepted_dir.mkdir(parents=True)
        source_target = inputs / "target.toml"
        source_peer = inputs / "peer.toml"
        accepted_target = accepted_dir / "target.toml"
        accepted_peer = accepted_dir / "peer.toml"
        for source, input_copy, accepted_copy in (
            (request.target_config, source_target, accepted_target),
            (request.peer_config, source_peer, accepted_peer),
        ):
            shutil.copyfile(source, input_copy)
            shutil.copyfile(source, accepted_copy)
        config_tool.validate_pair(source_target, source_peer)
        target_document = config_tool.dump(source_target)
        peer_document = config_tool.dump(source_peer)
        settings = _settings_from_config(
            target_document,
            max_iterations=request.max_iterations,
            target_binary=request.target_binary,
            peer_binary=request.peer_binary,
            configure_sha256=hashlib.sha256(
                request.configure_binary.read_bytes()
            ).hexdigest(),
        )
        peer_settings = _settings_from_config(
            peer_document,
            max_iterations=request.max_iterations,
            target_binary=request.target_binary,
            peer_binary=request.peer_binary,
            configure_sha256=settings.configure_sha256,
        )
        if settings != peer_settings:
            raise ApplicationError("target and peer tuning policies must match")
        target = config_tool.resolve(
            endpoint_id="target",
            config_path=source_target,
            binary_override=request.target_binary,
        )
        peer = config_tool.resolve(
            endpoint_id="peer",
            config_path=source_peer,
            binary_override=request.peer_binary,
        )
        if {target.spec.role, peer.spec.role} != {"client", "server"}:
            raise ApplicationError("target and peer require one client and one server")
        if target.spec.backend != peer.spec.backend:
            raise ApplicationError("target and peer backends must match")
        target_fingerprints = _probe_endpoint(
            session_id=session_id,
            endpoint=target,
            source_config=source_target,
            config_tool=config_tool,
            transport_factory=transport_factory,
        )
        peer_fingerprints = _probe_endpoint(
            session_id=session_id,
            endpoint=peer,
            source_config=source_peer,
            config_tool=config_tool,
            transport_factory=transport_factory,
        )
        identity = SessionIdentity(
            target=_endpoint_identity(
                root=stage,
                source=source_target,
                resolved=target,
                document=target_document,
                fingerprints=target_fingerprints,
            ),
            peer=_endpoint_identity(
                root=stage,
                source=source_peer,
                resolved=peer,
                document=peer_document,
                fingerprints=peer_fingerprints,
            ),
            metrics_schema="axio.metrics/v1",
            host_metrics_schema="pipetune.host-metrics/v1",
            providers=PROVIDER_NAMES,
        )
        accepted = ConfigPair(
            target=artifact_ref(stage, accepted_target),
            peer=artifact_ref(stage, accepted_peer),
        )
        TuningSessionStore(stage).create(
            session_id=session_id,
            identity=identity,
            accepted=accepted,
            details={"bootstrap": _settings_document(settings), "round": 0},
        )
        os.replace(stage, output)
        return output
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise


def _verify_live_identity(
    store: TuningSessionStore,
    state: SessionState,
    *,
    config_tool: AxioConfigTool,
    settings: _Settings,
    transport_factory: Callable[..., EndpointTransport],
) -> None:
    for endpoint_id, expected, reference, binary_override in (
        (
            "target",
            state.identity.target,
            state.accepted.target,
            settings.target_binary,
        ),
        ("peer", state.identity.peer, state.accepted.peer, settings.peer_binary),
    ):
        config = store.verify_artifact(reference)
        resolved = config_tool.resolve(
            endpoint_id=endpoint_id,
            config_path=config,
            binary_override=binary_override,
        )
        current = _probe_endpoint(
            session_id=state.session_id,
            endpoint=resolved,
            source_config=config,
            config_tool=config_tool,
            transport_factory=transport_factory,
        )
        if (
            current.git_commit != expected.fingerprints.git_commit
            or current.binary_sha256 != expected.fingerprints.binary_sha256
            or current.build != expected.fingerprints.build
        ):
            raise ApplicationError(f"{endpoint_id} live build identity changed")


def _run(
    root: pathlib.Path,
    *,
    configure_binary: pathlib.Path,
    config_tool: AxioConfigTool | None,
    transport_factory: Callable[..., EndpointTransport],
    verify_live_identity: bool = True,
) -> ApplicationResult:
    store = TuningSessionStore(root)
    state = store.status()
    settings = _settings_from_state(state.details.get("bootstrap"))
    try:
        configure_sha256 = hashlib.sha256(configure_binary.read_bytes()).hexdigest()
    except OSError as error:
        raise ApplicationError(f"cannot read axio-configure: {error}") from error
    if configure_sha256 != settings.configure_sha256:
        raise ApplicationError("controller axio-configure identity changed")
    tool = config_tool or AxioConfigTool(configure_binary)
    if verify_live_identity and state.phase != "complete":
        _verify_live_identity(
            store,
            state,
            config_tool=tool,
            settings=settings,
            transport_factory=transport_factory,
        )
    request = MeasureRequest(
        target_config=store.verify_artifact(state.accepted.target),
        peer_config=store.verify_artifact(state.accepted.peer),
        output=root / ".runner-placeholder",
        configure_binary=configure_binary,
        target_binary=settings.target_binary,
        peer_binary=settings.peer_binary,
    )
    executor = MeasureTrialExecutor(
        request=request,
        config_tool=tool,
        transport_factory=transport_factory,
    )
    controller = ColdStartController(
        root=root,
        store=store,
        config_tool=tool,
        executor=executor,
        policy=settings.policy,
        candidate_generator=generate_lock_averse_candidates,
        infrastructure_failure_limit=settings.infrastructure_failure_limit,
    )
    result = TuningLoop(
        store=store,
        round_runner=controller,
        policy=settings.policy,
    ).run(state, max_iterations=settings.max_iterations)
    publish_session_outputs(root, store, result)
    return ApplicationResult(status_document(root, store))


def bootstrap_session(
    request: BootstrapRequest,
    *,
    config_tool: AxioConfigTool | None = None,
    transport_factory: Callable[..., EndpointTransport] = transport_for,
    session_id_factory: Callable[[], str] = lambda: f"tuning-{uuid.uuid4().hex}",
) -> ApplicationResult:
    """Create one immutable session identity, then run bounded cold starts."""

    try:
        tool = config_tool or AxioConfigTool(request.configure_binary)
        root = _initialize(
            request,
            config_tool=tool,
            transport_factory=transport_factory,
            session_id=session_id_factory(),
        )
        return _run(
            root,
            configure_binary=request.configure_binary,
            config_tool=tool,
            transport_factory=transport_factory,
            verify_live_identity=False,
        )
    except ApplicationError:
        raise
    except Exception as error:
        raise ApplicationError(str(error)) from error


def resume_session(
    request: ResumeRequest,
    *,
    config_tool: AxioConfigTool | None = None,
    transport_factory: Callable[..., EndpointTransport] = transport_for,
) -> ApplicationResult:
    """Verify and continue a bounded session from its immutable cursor."""

    try:
        return _run(
            request.session.resolve(strict=True),
            configure_binary=request.configure_binary,
            config_tool=config_tool,
            transport_factory=transport_factory,
        )
    except ApplicationError:
        raise
    except Exception as error:
        raise ApplicationError(str(error)) from error


def read_session_status(session: pathlib.Path) -> ApplicationResult:
    """Inspect a session without cleanup, repair, probing, or publication."""

    try:
        root = session.resolve(strict=True)
        return ApplicationResult(status_document(root, TuningSessionStore(root)))
    except ApplicationError:
        raise
    except Exception as error:
        raise ApplicationError(str(error)) from error


__all__ = [
    "ApplicationError",
    "ApplicationResult",
    "BootstrapRequest",
    "ResumeRequest",
    "bootstrap_session",
    "read_session_status",
    "resume_session",
]
