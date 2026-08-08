"""Deterministic, atomic PipeTune artifact serialization and validation."""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import tempfile
from typing import Any, Iterable

from pipetune.model import (
    ArtifactRef,
    ContractError,
    CounterValue,
    EndpointSpec,
    FingerprintSet,
    MetricSample,
    ProcessResult,
    ProviderStatus,
    SessionManifest,
    TrialEndpoint,
    TrialManifest,
)


def _require(condition: bool, location: str, message: str) -> None:
    if not condition:
        raise ContractError(f"{location}: {message}")


def _unique_object(pairs: Iterable[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ContractError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ContractError(f"non-finite JSON constant {value}")


def _document(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            parse_constant=_reject_constant,
            object_pairs_hook=_unique_object,
        )
    except (json.JSONDecodeError, ContractError) as error:
        raise ContractError(f"{path}: invalid JSON: {error}") from error
    _require(isinstance(value, dict), str(path), "must contain a JSON object")
    return value


def _object(value: object, location: str) -> dict[str, Any]:
    _require(isinstance(value, dict), location, "must be an object")
    return value


def _array(value: object, location: str) -> list[object]:
    _require(isinstance(value, list), location, "must be an array")
    return value


def _keys(value: dict[str, Any], expected: set[str], location: str) -> None:
    missing = sorted(expected - value.keys())
    unknown = sorted(value.keys() - expected)
    _require(not missing, location, f"missing required keys {missing}")
    _require(not unknown, location, f"unknown keys {unknown}")


def _string(value: object, location: str, *, nullable: bool = False) -> str | None:
    if nullable and value is None:
        return None
    _require(isinstance(value, str), location, "must be a string")
    return value


def _integer(value: object, location: str, *, nullable: bool = False) -> int | None:
    if nullable and value is None:
        return None
    _require(type(value) is int, location, "must be an integer")
    return value


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            chunk = source.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def artifact_ref(
    root: pathlib.Path, path: pathlib.Path, *, schema: str | None = None
) -> ArtifactRef:
    root_resolved = root.resolve()
    path_resolved = path.resolve(strict=True)
    try:
        relative = path_resolved.relative_to(root_resolved)
    except ValueError as error:
        raise ContractError(f"artifact {path} is outside {root}") from error
    _require(path_resolved.is_file(), str(path), "artifact must be a regular file")
    return ArtifactRef(
        path=relative.as_posix(),
        sha256=sha256_file(path_resolved),
        size_bytes=path_resolved.stat().st_size,
        schema=schema,
    )


def write_json_atomic(path: pathlib.Path, document: object) -> None:
    try:
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
    except (TypeError, ValueError) as error:
        raise ContractError(f"{path}: JSON serialization failed: {error}") from error
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


def _artifact_document(value: ArtifactRef) -> dict[str, object]:
    return {
        "path": value.path,
        "schema": value.schema,
        "sha256": value.sha256,
        "size_bytes": value.size_bytes,
    }


def _artifact_value(value: object, location: str) -> ArtifactRef:
    document = _object(value, location)
    _keys(document, {"path", "schema", "sha256", "size_bytes"}, location)
    return ArtifactRef(
        path=_string(document["path"], f"{location}.path") or "",
        schema=_string(document["schema"], f"{location}.schema", nullable=True),
        sha256=_string(document["sha256"], f"{location}.sha256") or "",
        size_bytes=_integer(document["size_bytes"], f"{location}.size_bytes") or 0,
    )


def _counter_document(value: CounterValue) -> dict[str, object]:
    return {
        "available": value.available,
        "denominator": value.denominator,
        "name": value.name,
        "numerator": value.numerator,
        "rate_percent": value.rate_percent,
        "reason": value.reason,
    }


def _number(
    value: object, location: str, *, nullable: bool = False
) -> float | None:
    if nullable and value is None:
        return None
    _require(
        type(value) in (int, float),
        location,
        "must be a number",
    )
    return float(value)


def _counter_value(value: object, location: str) -> CounterValue:
    document = _object(value, location)
    _keys(
        document,
        {
            "available",
            "denominator",
            "name",
            "numerator",
            "rate_percent",
            "reason",
        },
        location,
    )
    available = document["available"]
    _require(type(available) is bool, f"{location}.available", "must be a boolean")
    return CounterValue(
        name=_string(document["name"], f"{location}.name") or "",
        available=available,
        numerator=_number(
            document["numerator"], f"{location}.numerator", nullable=True
        ),
        denominator=_number(
            document["denominator"], f"{location}.denominator", nullable=True
        ),
        rate_percent=_number(
            document["rate_percent"], f"{location}.rate_percent", nullable=True
        ),
        reason=_string(document["reason"], f"{location}.reason", nullable=True),
    )


def _provider_document(value: ProviderStatus) -> dict[str, object]:
    return {
        "available": value.available,
        "name": value.name,
        "path": value.path,
        "reason": value.reason,
        "version": value.version,
    }


def _provider_value(value: object, location: str) -> ProviderStatus:
    document = _object(value, location)
    _keys(document, {"available", "name", "path", "reason", "version"}, location)
    available = document["available"]
    _require(type(available) is bool, f"{location}.available", "must be a boolean")
    return ProviderStatus(
        name=_string(document["name"], f"{location}.name") or "",
        available=available,
        path=_string(document["path"], f"{location}.path", nullable=True),
        version=_string(document["version"], f"{location}.version", nullable=True),
        reason=_string(document["reason"], f"{location}.reason", nullable=True),
    )


def metric_sample_document(value: MetricSample) -> dict[str, object]:
    return {
        "commands": [_process_document(command) for command in value.commands],
        "counters": [_counter_document(counter) for counter in value.counters],
        "ended_at_utc": value.ended_at_utc,
        "endpoint_id": value.endpoint_id,
        "providers": [
            _provider_document(provider) for provider in value.providers
        ],
        "raw_artifacts": [
            _artifact_document(artifact) for artifact in value.raw_artifacts
        ],
        "sample_interval_seconds": value.sample_interval_seconds,
        "sample_id": value.sample_id,
        "schema": value.schema,
        "socket_id": value.socket_id,
        "started_at_utc": value.started_at_utc,
    }


def _metric_sample_value(value: object, location: str) -> MetricSample:
    document = _object(value, location)
    _keys(
        document,
        {
            "counters",
            "commands",
            "ended_at_utc",
            "endpoint_id",
            "providers",
            "raw_artifacts",
            "sample_interval_seconds",
            "sample_id",
            "schema",
            "socket_id",
            "started_at_utc",
        },
        location,
    )
    return MetricSample(
        schema=_string(document["schema"], f"{location}.schema") or "",
        sample_id=_string(document["sample_id"], f"{location}.sample_id") or "",
        endpoint_id=_string(
            document["endpoint_id"], f"{location}.endpoint_id"
        ) or "",
        started_at_utc=_string(
            document["started_at_utc"], f"{location}.started_at_utc"
        ) or "",
        ended_at_utc=_string(
            document["ended_at_utc"], f"{location}.ended_at_utc"
        ) or "",
        sample_interval_seconds=_number(
            document["sample_interval_seconds"],
            f"{location}.sample_interval_seconds",
        )
        or 0.0,
        socket_id=_integer(document["socket_id"], f"{location}.socket_id") or 0,
        counters=tuple(
            _counter_value(counter, f"{location}.counters[{index}]")
            for index, counter in enumerate(
                _array(document["counters"], f"{location}.counters")
            )
        ),
        commands=tuple(
            _process_value(command, f"{location}.commands[{index}]")
            for index, command in enumerate(
                _array(document["commands"], f"{location}.commands")
            )
        ),
        providers=tuple(
            _provider_value(provider, f"{location}.providers[{index}]")
            for index, provider in enumerate(
                _array(document["providers"], f"{location}.providers")
            )
        ),
        raw_artifacts=tuple(
            _artifact_value(artifact, f"{location}.raw_artifacts[{index}]")
            for index, artifact in enumerate(
                _array(document["raw_artifacts"], f"{location}.raw_artifacts")
            )
        ),
    )


def write_metric_sample(path: pathlib.Path, sample: MetricSample) -> None:
    write_json_atomic(path, metric_sample_document(sample))


def load_metric_sample(
    path: pathlib.Path, *, artifact_root: pathlib.Path | None = None
) -> MetricSample:
    sample = _metric_sample_value(_document(path), str(path))
    if artifact_root is not None:
        for reference in sample.raw_artifacts:
            _verify_artifact(artifact_root.resolve(), reference)
    return sample


def _endpoint_spec_document(value: EndpointSpec) -> dict[str, object]:
    return {
        "backend": value.backend,
        "endpoint_id": value.endpoint_id,
        "host": value.host,
        "numa_node": value.numa_node,
        "role": value.role,
        "ssh_port": value.ssh_port,
        "ssh_user": value.ssh_user,
        "transport": value.transport,
        "use_sudo": value.use_sudo,
        "workdir": value.workdir,
    }


def _endpoint_spec_value(value: object, location: str) -> EndpointSpec:
    document = _object(value, location)
    expected = {
        "backend",
        "endpoint_id",
        "host",
        "numa_node",
        "role",
        "ssh_port",
        "ssh_user",
        "transport",
        "use_sudo",
        "workdir",
    }
    _keys(document, expected, location)
    use_sudo = document["use_sudo"]
    _require(type(use_sudo) is bool, f"{location}.use_sudo", "must be a boolean")
    return EndpointSpec(
        endpoint_id=_string(document["endpoint_id"], f"{location}.endpoint_id") or "",
        role=_string(document["role"], f"{location}.role") or "",
        backend=_string(document["backend"], f"{location}.backend") or "",
        transport=_string(document["transport"], f"{location}.transport") or "",
        host=_string(document["host"], f"{location}.host") or "",
        ssh_port=_integer(document["ssh_port"], f"{location}.ssh_port") or 0,
        ssh_user=_string(document["ssh_user"], f"{location}.ssh_user") or "",
        workdir=_string(document["workdir"], f"{location}.workdir") or "",
        use_sudo=use_sudo,
        numa_node=_integer(document["numa_node"], f"{location}.numa_node") or 0,
    )


def _fingerprints_document(value: FingerprintSet) -> dict[str, object]:
    return {
        "binary_sha256": value.binary_sha256,
        "build": value.build,
        "datapath": value.datapath,
        "deployment": value.deployment,
        "git_commit": value.git_commit,
        "source_config_sha256": value.source_config_sha256,
    }


def _fingerprints_value(value: object, location: str) -> FingerprintSet:
    document = _object(value, location)
    expected = {
        "binary_sha256",
        "build",
        "datapath",
        "deployment",
        "git_commit",
        "source_config_sha256",
    }
    _keys(document, expected, location)
    return FingerprintSet(
        git_commit=_string(document["git_commit"], f"{location}.git_commit") or "",
        binary_sha256=_string(
            document["binary_sha256"], f"{location}.binary_sha256"
        ) or "",
        source_config_sha256=_string(
            document["source_config_sha256"],
            f"{location}.source_config_sha256",
        ) or "",
        build=_string(document["build"], f"{location}.build") or "",
        datapath=_string(document["datapath"], f"{location}.datapath") or "",
        deployment=_string(document["deployment"], f"{location}.deployment") or "",
    )


def _process_document(value: ProcessResult) -> dict[str, object]:
    return {
        "argv": list(value.argv),
        "ended_at_utc": value.ended_at_utc,
        "failure_reason": value.failure_reason,
        "return_code": value.return_code,
        "status": value.status,
        "started_at_utc": value.started_at_utc,
        "stderr": _artifact_document(value.stderr),
        "stdout": _artifact_document(value.stdout),
    }


def _process_value(value: object, location: str) -> ProcessResult:
    document = _object(value, location)
    expected = {
        "argv",
        "ended_at_utc",
        "failure_reason",
        "return_code",
        "status",
        "started_at_utc",
        "stderr",
        "stdout",
    }
    _keys(document, expected, location)
    argv_values = _array(document["argv"], f"{location}.argv")
    argv = tuple(
        _string(argument, f"{location}.argv[{index}]") or ""
        for index, argument in enumerate(argv_values)
    )
    return ProcessResult(
        argv=argv,
        status=_string(document["status"], f"{location}.status") or "",
        return_code=_integer(
            document["return_code"], f"{location}.return_code", nullable=True
        ),
        failure_reason=_string(
            document["failure_reason"],
            f"{location}.failure_reason",
            nullable=True,
        ),
        started_at_utc=_string(
            document["started_at_utc"], f"{location}.started_at_utc"
        ) or "",
        ended_at_utc=_string(
            document["ended_at_utc"], f"{location}.ended_at_utc"
        ) or "",
        stdout=_artifact_value(document["stdout"], f"{location}.stdout"),
        stderr=_artifact_value(document["stderr"], f"{location}.stderr"),
    )


def trial_manifest_document(value: TrialManifest) -> dict[str, object]:
    endpoints = []
    for endpoint in value.endpoints:
        endpoints.append(
            {
                "artifacts": [
                    _artifact_document(artifact) for artifact in endpoint.artifacts
                ],
                "fingerprints": _fingerprints_document(endpoint.fingerprints),
                "process": _process_document(endpoint.process),
                "spec": _endpoint_spec_document(endpoint.spec),
            }
        )
    return {
        "cleanup_status": value.cleanup_status,
        "ended_at_utc": value.ended_at_utc,
        "endpoints": endpoints,
        "failure_reason": value.failure_reason,
        "host_metrics": (
            _artifact_document(value.host_metrics)
            if value.host_metrics is not None
            else None
        ),
        "schema": value.schema,
        "started_at_utc": value.started_at_utc,
        "status": value.status,
        "target_endpoint_id": value.target_endpoint_id,
        "trial_id": value.trial_id,
    }


def _trial_manifest_value(value: object, location: str) -> TrialManifest:
    document = _object(value, location)
    expected = {
        "cleanup_status",
        "ended_at_utc",
        "endpoints",
        "failure_reason",
        "host_metrics",
        "schema",
        "started_at_utc",
        "status",
        "target_endpoint_id",
        "trial_id",
    }
    _keys(document, expected, location)
    endpoints = []
    for index, raw_endpoint in enumerate(
        _array(document["endpoints"], f"{location}.endpoints")
    ):
        endpoint_location = f"{location}.endpoints[{index}]"
        endpoint = _object(raw_endpoint, endpoint_location)
        _keys(endpoint, {"artifacts", "fingerprints", "process", "spec"}, endpoint_location)
        endpoints.append(
            TrialEndpoint(
                spec=_endpoint_spec_value(endpoint["spec"], f"{endpoint_location}.spec"),
                fingerprints=_fingerprints_value(
                    endpoint["fingerprints"], f"{endpoint_location}.fingerprints"
                ),
                process=_process_value(
                    endpoint["process"], f"{endpoint_location}.process"
                ),
                artifacts=tuple(
                    _artifact_value(artifact, f"{endpoint_location}.artifacts[{artifact_index}]")
                    for artifact_index, artifact in enumerate(
                        _array(endpoint["artifacts"], f"{endpoint_location}.artifacts")
                    )
                ),
            )
        )
    host_metrics_raw = document["host_metrics"]
    return TrialManifest(
        schema=_string(document["schema"], f"{location}.schema") or "",
        trial_id=_string(document["trial_id"], f"{location}.trial_id") or "",
        target_endpoint_id=_string(
            document["target_endpoint_id"], f"{location}.target_endpoint_id"
        ) or "",
        started_at_utc=_string(
            document["started_at_utc"], f"{location}.started_at_utc"
        ) or "",
        ended_at_utc=_string(
            document["ended_at_utc"], f"{location}.ended_at_utc"
        ) or "",
        status=_string(document["status"], f"{location}.status") or "",
        cleanup_status=_string(
            document["cleanup_status"], f"{location}.cleanup_status"
        ) or "",
        endpoints=tuple(endpoints),
        host_metrics=(
            None
            if host_metrics_raw is None
            else _artifact_value(host_metrics_raw, f"{location}.host_metrics")
        ),
        failure_reason=_string(
            document["failure_reason"], f"{location}.failure_reason", nullable=True
        ),
    )


def _artifact_references(manifest: TrialManifest) -> tuple[ArtifactRef, ...]:
    references: list[ArtifactRef] = []
    for endpoint in manifest.endpoints:
        references.extend((endpoint.process.stdout, endpoint.process.stderr))
        references.extend(endpoint.artifacts)
    if manifest.host_metrics is not None:
        references.append(manifest.host_metrics)
    return tuple(references)


def _verify_artifact(root: pathlib.Path, reference: ArtifactRef) -> None:
    path = root / pathlib.PurePosixPath(reference.path)
    _require(path.is_file(), reference.path, "artifact is missing")
    _require(path.stat().st_size == reference.size_bytes, reference.path, "size mismatch")
    _require(sha256_file(path) == reference.sha256, reference.path, "SHA-256 mismatch")


def write_trial_manifest(path: pathlib.Path, manifest: TrialManifest) -> None:
    write_json_atomic(path, trial_manifest_document(manifest))


def load_trial_manifest(
    path: pathlib.Path, *, artifact_root: pathlib.Path | None = None
) -> TrialManifest:
    manifest = _trial_manifest_value(_document(path), str(path))
    if artifact_root is not None:
        for reference in _artifact_references(manifest):
            _verify_artifact(artifact_root.resolve(), reference)
    return manifest


def session_manifest_document(value: SessionManifest) -> dict[str, object]:
    return {
        "ended_at_utc": value.ended_at_utc,
        "failure_reason": value.failure_reason,
        "schema": value.schema,
        "session_id": value.session_id,
        "started_at_utc": value.started_at_utc,
        "status": value.status,
        "trials": [_artifact_document(trial) for trial in value.trials],
    }


def _session_manifest_value(value: object, location: str) -> SessionManifest:
    document = _object(value, location)
    _keys(
        document,
        {
            "ended_at_utc",
            "failure_reason",
            "schema",
            "session_id",
            "started_at_utc",
            "status",
            "trials",
        },
        location,
    )
    return SessionManifest(
        schema=_string(document["schema"], f"{location}.schema") or "",
        session_id=_string(document["session_id"], f"{location}.session_id") or "",
        started_at_utc=_string(
            document["started_at_utc"], f"{location}.started_at_utc"
        )
        or "",
        ended_at_utc=_string(
            document["ended_at_utc"], f"{location}.ended_at_utc", nullable=True
        ),
        status=_string(document["status"], f"{location}.status") or "",
        trials=tuple(
            _artifact_value(trial, f"{location}.trials[{index}]")
            for index, trial in enumerate(
                _array(document["trials"], f"{location}.trials")
            )
        ),
        failure_reason=_string(
            document["failure_reason"], f"{location}.failure_reason", nullable=True
        ),
    )


def write_session_manifest(path: pathlib.Path, manifest: SessionManifest) -> None:
    write_json_atomic(path, session_manifest_document(manifest))


def load_session_manifest(
    path: pathlib.Path, *, artifact_root: pathlib.Path | None = None
) -> SessionManifest:
    manifest = _session_manifest_value(_document(path), str(path))
    if artifact_root is not None:
        for reference in manifest.trials:
            _verify_artifact(artifact_root.resolve(), reference)
    return manifest
