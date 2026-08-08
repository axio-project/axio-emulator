"""Atomic, immutable, and resumable state for PipeTune tuning sessions."""

from __future__ import annotations

import dataclasses
import hashlib
import json
import math
import os
import pathlib
import re
import tempfile
from typing import Any, Iterable

from pipetune.artifacts import (
    artifact_ref,
    load_metric_sample,
    load_trial_manifest,
    sha256_file,
    write_json_atomic,
)
from pipetune.metrics import load_axio_jsonl
from pipetune.model import (
    ArtifactRef,
    ContractError,
    FingerprintSet,
    PROVIDER_NAMES,
    SHA256_PATTERN,
)


POINTER_SCHEMA = "pipetune.tuning-session-pointer/v1"
STATE_SCHEMA = "pipetune.tuning-session-state/v1"
PHASES = (
    "baseline",
    "diagnose",
    "probe",
    "candidates",
    "select",
    "accepted",
    "rolled_back",
    "complete",
)
PHASE_TRANSITIONS = {
    "baseline": frozenset(("diagnose", "complete")),
    "diagnose": frozenset(("probe", "candidates", "rolled_back", "complete")),
    "probe": frozenset(("diagnose", "rolled_back", "complete")),
    "candidates": frozenset(("select", "rolled_back", "complete")),
    "select": frozenset(("accepted", "rolled_back", "complete")),
    "accepted": frozenset(("diagnose", "complete")),
    "rolled_back": frozenset(("diagnose", "complete")),
    "complete": frozenset(),
}
ATTEMPT_STATUSES = ("pending", "running", "complete", "abandoned")
TRIAL_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")


class SessionError(RuntimeError):
    """Raised when persisted tuning state cannot be trusted or advanced."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise SessionError(message)


def _json_bytes(document: object) -> bytes:
    try:
        return (
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
        raise SessionError(f"session JSON serialization failed: {error}") from error


def _sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _reject_constant(value: str) -> None:
    raise SessionError(f"non-finite JSON constant {value}")


def _unique_object(pairs: Iterable[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise SessionError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _document_from_bytes(payload: bytes, location: object) -> dict[str, Any]:
    try:
        value = json.loads(
            payload.decode("utf-8", errors="strict"),
            parse_constant=_reject_constant,
            object_pairs_hook=_unique_object,
        )
    except (json.JSONDecodeError, UnicodeError, SessionError) as error:
        raise SessionError(f"{location}: invalid session JSON: {error}") from error
    _require(isinstance(value, dict), f"{location}: must contain an object")
    return value


def _load_document(path: pathlib.Path) -> dict[str, Any]:
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise SessionError(f"{path}: cannot read session JSON: {error}") from error
    return _document_from_bytes(payload, path)


def _keys(document: dict[str, Any], expected: set[str], location: str) -> None:
    missing = sorted(expected - document.keys())
    unknown = sorted(document.keys() - expected)
    _require(not missing, f"{location}: missing keys {missing}")
    _require(not unknown, f"{location}: unknown keys {unknown}")


def _object(value: object, location: str) -> dict[str, Any]:
    _require(isinstance(value, dict), f"{location}: must be an object")
    return value


def _array(value: object, location: str) -> list[object]:
    _require(isinstance(value, list), f"{location}: must be an array")
    return value


def _string(value: object, location: str, *, nullable: bool = False) -> str | None:
    if nullable and value is None:
        return None
    _require(isinstance(value, str), f"{location}: must be a string")
    return value


def _integer(value: object, location: str) -> int:
    _require(type(value) is int, f"{location}: must be an integer")
    return int(value)


def _boolean(value: object, location: str) -> bool:
    _require(type(value) is bool, f"{location}: must be a boolean")
    return bool(value)


def _optional_string(value: object, location: str) -> str | None:
    result = _string(value, location, nullable=True)
    _require(result is None or bool(result), f"{location}: must not be empty")
    return result


def _optional_number(value: object, location: str) -> float | None:
    if value is None:
        return None
    _require(type(value) in (int, float), f"{location}: must be a number or null")
    result = float(value)
    _require(math.isfinite(result), f"{location}: must be finite")
    return result


def _validate_details(value: object, location: str = "details") -> None:
    if value is None or isinstance(value, (str, bool)):
        return
    if type(value) is int:
        return
    if type(value) is float:
        _require(math.isfinite(value), f"{location}: must be finite")
        return
    if isinstance(value, list):
        for index, item in enumerate(value):
            _validate_details(item, f"{location}[{index}]")
        return
    if isinstance(value, dict):
        for key, item in value.items():
            _require(isinstance(key, str), f"{location}: keys must be strings")
            _validate_details(item, f"{location}.{key}")
        return
    raise SessionError(f"{location}: unsupported JSON value {type(value).__name__}")


def _validate_topology_evidence(value: object, location: str) -> None:
    document = _object(value, location)
    _keys(
        document,
        {
            "application_count",
            "dispatcher_count",
            "overlap_count",
            "physical_core_count",
            "physical_core_budget",
            "fanout",
        },
        location,
    )
    application_count = _integer(
        document["application_count"], f"{location}.application_count"
    )
    dispatcher_count = _integer(
        document["dispatcher_count"], f"{location}.dispatcher_count"
    )
    overlap_count = _integer(
        document["overlap_count"], f"{location}.overlap_count"
    )
    physical_core_count = _integer(
        document["physical_core_count"], f"{location}.physical_core_count"
    )
    physical_core_budget = _integer(
        document["physical_core_budget"], f"{location}.physical_core_budget"
    )
    _require(
        application_count > 0 and dispatcher_count > 0,
        f"{location}: active counts must be positive",
    )
    _require(
        0 <= overlap_count <= min(application_count, dispatcher_count),
        f"{location}: overlap count is invalid",
    )
    _require(
        0 < physical_core_count <= physical_core_budget,
        f"{location}: physical core counts are invalid",
    )
    fanout = _array(document["fanout"], f"{location}.fanout")
    dispatchers: set[int] = set()
    applications = 0
    for index, item in enumerate(fanout):
        item_location = f"{location}.fanout[{index}]"
        entry = _object(item, item_location)
        _keys(entry, {"dispatcher", "applications"}, item_location)
        dispatcher = _integer(entry["dispatcher"], f"{item_location}.dispatcher")
        count = _integer(entry["applications"], f"{item_location}.applications")
        _require(dispatcher not in dispatchers, f"{location}: duplicate dispatcher")
        _require(count >= 0, f"{item_location}.applications: must be non-negative")
        dispatchers.add(dispatcher)
        applications += count
    _require(
        len(dispatchers) == dispatcher_count and applications == application_count,
        f"{location}: fanout does not match active counts",
    )


def _validate_impact_spec(value: object, location: str) -> None:
    document = _object(value, location)
    _keys(document, {"kind", "metric", "direction"}, location)
    kind = _string(document["kind"], f"{location}.kind")
    _require(kind in ("diagnosis", "component"), f"{location}.kind: invalid value")
    metric = _optional_string(document["metric"], f"{location}.metric")
    direction = _optional_string(document["direction"], f"{location}.direction")
    _require(direction in (None, "rx", "tx"), f"{location}.direction: invalid value")
    _require(
        kind != "component" or metric is not None,
        f"{location}: component needs a metric",
    )


def _validate_expected_impact(value: object, location: str) -> bool:
    document = _object(value, location)
    _keys(
        document,
        {
            "candidate_id",
            "point",
            "metric",
            "accepted",
            "reason",
            "baseline_value",
            "candidate_value",
            "observed_reduction",
            "required_reduction",
            "unit",
        },
        location,
    )
    for name in ("candidate_id", "point", "metric", "reason"):
        _require(
            bool(_string(document[name], f"{location}.{name}")),
            f"{location}.{name}: must not be empty",
        )
    for name in (
        "baseline_value",
        "candidate_value",
        "observed_reduction",
        "required_reduction",
    ):
        _optional_number(document[name], f"{location}.{name}")
    _optional_string(document["unit"], f"{location}.unit")
    return _boolean(document["accepted"], f"{location}.accepted")


def _validate_objective_comparison(value: object, location: str) -> bool:
    document = _object(value, location)
    _keys(
        document,
        {
            "candidate_id",
            "accepted",
            "reason",
            "metric",
            "observed_improvement",
            "required_improvement",
            "accepted_feasible",
            "candidate_feasible",
            "acceptance_mode",
            "physical_core_delta",
        },
        location,
    )
    for name in ("candidate_id", "reason"):
        _require(
            bool(_string(document[name], f"{location}.{name}")),
            f"{location}.{name}: must not be empty",
        )
    _optional_string(document["metric"], f"{location}.metric")
    for name in ("observed_improvement", "required_improvement"):
        _optional_number(document[name], f"{location}.{name}")
    _boolean(document["accepted_feasible"], f"{location}.accepted_feasible")
    candidate_feasible = document["candidate_feasible"]
    _require(
        candidate_feasible is None or type(candidate_feasible) is bool,
        f"{location}.candidate_feasible: must be a boolean or null",
    )
    acceptance_mode = _optional_string(
        document["acceptance_mode"], f"{location}.acceptance_mode"
    )
    _require(
        acceptance_mode
        in (
            None,
            "significant_latency",
            "significant_throughput",
            "equivalent_fewer_cores",
        ),
        f"{location}.acceptance_mode: invalid value",
    )
    physical_delta = document["physical_core_delta"]
    _require(
        physical_delta is None or type(physical_delta) is int,
        f"{location}.physical_core_delta: must be an integer or null",
    )
    return _boolean(document["accepted"], f"{location}.accepted")


def _validate_candidate_evaluation(value: object, location: str) -> None:
    document = _object(value, location)
    _keys(
        document,
        {
            "candidate_id",
            "action",
            "kind",
            "profile",
            "search_phase",
            "impact",
            "source_topology",
            "candidate_topology",
            "target_sha256",
            "peer_sha256",
            "trial_id",
            "expected_impact",
            "objective",
            "valid",
            "reused_probe",
            "reused_visited",
            "rejection_reason",
        },
        location,
    )
    for name in ("candidate_id", "action", "kind", "trial_id"):
        _require(
            bool(_string(document[name], f"{location}.{name}")),
            f"{location}.{name}: must not be empty",
        )
    _optional_string(document["profile"], f"{location}.profile")
    phase = _string(document["search_phase"], f"{location}.search_phase")
    _require(phase in ("memory", "compute"), f"{location}.search_phase: invalid value")
    _validate_impact_spec(document["impact"], f"{location}.impact")
    _validate_topology_evidence(
        document["source_topology"], f"{location}.source_topology"
    )
    _validate_topology_evidence(
        document["candidate_topology"], f"{location}.candidate_topology"
    )
    for name in ("target_sha256", "peer_sha256"):
        digest = _string(document[name], f"{location}.{name}") or ""
        _require(
            bool(SHA256_PATTERN.fullmatch(digest)),
            f"{location}.{name}: invalid SHA-256",
        )
    expected = _validate_expected_impact(
        document["expected_impact"], f"{location}.expected_impact"
    )
    objective = _validate_objective_comparison(
        document["objective"], f"{location}.objective"
    )
    valid = _boolean(document["valid"], f"{location}.valid")
    _require(
        valid == (expected and objective),
        f"{location}.valid: inconsistent gates",
    )
    _boolean(document["reused_probe"], f"{location}.reused_probe")
    _boolean(document["reused_visited"], f"{location}.reused_visited")
    rejection = _optional_string(
        document["rejection_reason"], f"{location}.rejection_reason"
    )
    _require(
        (rejection is None) == valid,
        f"{location}.rejection_reason: inconsistent decision",
    )


def _validate_tuning_evidence(details: dict[str, Any]) -> None:
    for name in ("candidate_evaluations", "visited_candidates"):
        if name not in details:
            continue
        values = _array(details[name], f"details.{name}")
        for index, value in enumerate(values):
            _validate_candidate_evaluation(value, f"details.{name}[{index}]")
    recovered = details.get("recovered_candidate_trials")
    if recovered is None:
        return
    records = _array(recovered, "details.recovered_candidate_trials")
    for index, value in enumerate(records):
        location = f"details.recovered_candidate_trials[{index}]"
        document = _object(value, location)
        _keys(
            document,
            {"target_sha256", "peer_sha256", "trial_id"},
            location,
        )
        for name in ("target_sha256", "peer_sha256"):
            digest = _string(document[name], f"{location}.{name}") or ""
            _require(
                bool(SHA256_PATTERN.fullmatch(digest)),
                f"{location}.{name}: invalid SHA-256",
            )
        trial_id = _string(document["trial_id"], f"{location}.trial_id")
        _require(
            bool(trial_id and TRIAL_ID_PATTERN.fullmatch(trial_id)),
            f"{location}.trial_id: invalid trial ID",
        )


@dataclasses.dataclass(frozen=True)
class EndpointIdentity:
    endpoint_id: str
    role: str
    source_config: ArtifactRef
    fingerprints: FingerprintSet
    canonical_config_sha256: str

    def __post_init__(self) -> None:
        _require(bool(self.endpoint_id), "endpoint identity ID must not be empty")
        _require(self.role in ("client", "server"), "endpoint identity role is invalid")
        _require(
            self.source_config.schema is None,
            "source config artifact must not claim a data schema",
        )
        _require(
            self.fingerprints.source_config_sha256 == self.source_config.sha256,
            "endpoint identity source config hash is inconsistent",
        )
        _require(
            bool(SHA256_PATTERN.fullmatch(self.canonical_config_sha256)),
            "canonical config SHA-256 is invalid",
        )


@dataclasses.dataclass(frozen=True)
class SessionIdentity:
    target: EndpointIdentity
    peer: EndpointIdentity
    metrics_schema: str
    host_metrics_schema: str
    providers: tuple[str, ...]

    def __post_init__(self) -> None:
        _require(
            self.target.endpoint_id != self.peer.endpoint_id,
            "target and peer endpoint IDs must differ",
        )
        _require(
            {self.target.role, self.peer.role} == {"client", "server"},
            "session identity requires one client and one server",
        )
        _require(
            self.metrics_schema == "axio.metrics/v1",
            "session metrics schema is unsupported",
        )
        _require(
            self.host_metrics_schema == "pipetune.host-metrics/v1",
            "session host metrics schema is unsupported",
        )
        _require(
            self.providers == PROVIDER_NAMES,
            "session providers must be ordered perf and pcm_pcie",
        )


@dataclasses.dataclass(frozen=True)
class ConfigPair:
    target: ArtifactRef
    peer: ArtifactRef

    def __post_init__(self) -> None:
        _require(self.target.path != self.peer.path, "config pair paths must differ")
        _require(
            self.target.schema is None and self.peer.schema is None,
            "config pair artifacts must not claim a data schema",
        )


@dataclasses.dataclass(frozen=True)
class TrialAttempt:
    trial_id: str
    status: str
    manifest_path: str
    manifest: ArtifactRef | None

    def __post_init__(self) -> None:
        _require(
            bool(TRIAL_ID_PATTERN.fullmatch(self.trial_id)),
            "trial attempt ID is invalid",
        )
        expected_path = f"trials/{self.trial_id}/trial.json"
        _require(
            self.manifest_path == expected_path,
            "trial attempt manifest path is invalid",
        )
        _require(self.status in ATTEMPT_STATUSES, "trial attempt status is invalid")
        if self.status == "complete":
            _require(self.manifest is not None, "complete trial needs a manifest")
            _require(
                self.manifest.path == self.manifest_path
                and self.manifest.schema == "pipetune.trial/v1",
                "complete trial manifest reference is invalid",
            )
        else:
            _require(
                self.manifest is None,
                "unfinished trial must not contain a manifest reference",
            )


@dataclasses.dataclass(frozen=True)
class SessionState:
    schema: str
    session_id: str
    generation: int
    phase: str
    previous_state_sha256: str | None
    identity: SessionIdentity
    accepted: ConfigPair
    attempts: tuple[TrialAttempt, ...]
    details: dict[str, Any]
    state_path: str
    state_sha256: str

    def __post_init__(self) -> None:
        _require(self.schema == STATE_SCHEMA, "tuning state schema is unsupported")
        _require(bool(self.session_id), "tuning session ID must not be empty")
        _require(self.generation >= 0, "tuning state generation must be non-negative")
        _require(self.phase in PHASES, "tuning state phase is invalid")
        if self.generation == 0:
            _require(
                self.previous_state_sha256 is None,
                "initial state must not have a previous hash",
            )
            _require(self.phase == "baseline", "initial state must be baseline")
        else:
            _require(
                self.previous_state_sha256 is not None
                and bool(SHA256_PATTERN.fullmatch(self.previous_state_sha256)),
                "non-initial state needs a previous SHA-256",
            )
        expected_path = f"state/generation-{self.generation:08d}.json"
        _require(self.state_path == expected_path, "tuning state path is invalid")
        _require(
            bool(SHA256_PATTERN.fullmatch(self.state_sha256)),
            "tuning state SHA-256 is invalid",
        )
        identifiers = tuple(attempt.trial_id for attempt in self.attempts)
        _require(len(set(identifiers)) == len(identifiers), "trial IDs must be unique")
        _require(
            sum(
                attempt.status in ("pending", "running")
                for attempt in self.attempts
            )
            <= 1,
            "only one active trial may be pending or running",
        )
        if self.phase == "complete":
            _require(
                not any(
                    attempt.status in ("pending", "running")
                    for attempt in self.attempts
                ),
                "complete state cannot contain unfinished trials",
            )
        _require(isinstance(self.details, dict), "details must be an object")
        _validate_details(self.details)
        _validate_tuning_evidence(self.details)


@dataclasses.dataclass(frozen=True)
class RecoveryResult:
    state: SessionState
    action: str
    trial_id: str | None

    def __post_init__(self) -> None:
        _require(
            self.action in ("none", "finalized", "rerun"),
            "recovery action is invalid",
        )
        if self.action == "none":
            _require(self.trial_id is None, "empty recovery must not name a trial")
        else:
            _require(bool(self.trial_id), "recovery action needs a trial ID")


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
    try:
        return ArtifactRef(
            path=_string(document["path"], f"{location}.path") or "",
            schema=_string(document["schema"], f"{location}.schema", nullable=True),
            sha256=_string(document["sha256"], f"{location}.sha256") or "",
            size_bytes=_integer(document["size_bytes"], f"{location}.size_bytes"),
        )
    except ContractError as error:
        raise SessionError(f"{location}: {error}") from error


def _fingerprints_document(value: FingerprintSet) -> dict[str, str]:
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
    try:
        return FingerprintSet(
            binary_sha256=_string(
                document["binary_sha256"], f"{location}.binary_sha256"
            )
            or "",
            build=_string(document["build"], f"{location}.build") or "",
            datapath=_string(document["datapath"], f"{location}.datapath") or "",
            deployment=_string(document["deployment"], f"{location}.deployment")
            or "",
            git_commit=_string(document["git_commit"], f"{location}.git_commit")
            or "",
            source_config_sha256=_string(
                document["source_config_sha256"],
                f"{location}.source_config_sha256",
            )
            or "",
        )
    except ContractError as error:
        raise SessionError(f"{location}: {error}") from error


def _endpoint_identity_document(value: EndpointIdentity) -> dict[str, object]:
    return {
        "canonical_config_sha256": value.canonical_config_sha256,
        "endpoint_id": value.endpoint_id,
        "fingerprints": _fingerprints_document(value.fingerprints),
        "role": value.role,
        "source_config": _artifact_document(value.source_config),
    }


def _endpoint_identity_value(value: object, location: str) -> EndpointIdentity:
    document = _object(value, location)
    expected = {
        "canonical_config_sha256",
        "endpoint_id",
        "fingerprints",
        "role",
        "source_config",
    }
    _keys(document, expected, location)
    return EndpointIdentity(
        endpoint_id=_string(document["endpoint_id"], f"{location}.endpoint_id") or "",
        role=_string(document["role"], f"{location}.role") or "",
        source_config=_artifact_value(
            document["source_config"], f"{location}.source_config"
        ),
        fingerprints=_fingerprints_value(
            document["fingerprints"], f"{location}.fingerprints"
        ),
        canonical_config_sha256=_string(
            document["canonical_config_sha256"],
            f"{location}.canonical_config_sha256",
        )
        or "",
    )


def _identity_document(value: SessionIdentity) -> dict[str, object]:
    return {
        "host_metrics_schema": value.host_metrics_schema,
        "metrics_schema": value.metrics_schema,
        "peer": _endpoint_identity_document(value.peer),
        "providers": list(value.providers),
        "target": _endpoint_identity_document(value.target),
    }


def _identity_value(value: object, location: str) -> SessionIdentity:
    document = _object(value, location)
    expected = {
        "host_metrics_schema",
        "metrics_schema",
        "peer",
        "providers",
        "target",
    }
    _keys(document, expected, location)
    return SessionIdentity(
        target=_endpoint_identity_value(document["target"], f"{location}.target"),
        peer=_endpoint_identity_value(document["peer"], f"{location}.peer"),
        metrics_schema=_string(
            document["metrics_schema"], f"{location}.metrics_schema"
        )
        or "",
        host_metrics_schema=_string(
            document["host_metrics_schema"], f"{location}.host_metrics_schema"
        )
        or "",
        providers=tuple(
            _string(item, f"{location}.providers[{index}]") or ""
            for index, item in enumerate(
                _array(document["providers"], f"{location}.providers")
            )
        ),
    )


def _config_pair_document(value: ConfigPair) -> dict[str, object]:
    return {
        "peer": _artifact_document(value.peer),
        "target": _artifact_document(value.target),
    }


def _config_pair_value(value: object, location: str) -> ConfigPair:
    document = _object(value, location)
    _keys(document, {"peer", "target"}, location)
    return ConfigPair(
        target=_artifact_value(document["target"], f"{location}.target"),
        peer=_artifact_value(document["peer"], f"{location}.peer"),
    )


def _attempt_document(value: TrialAttempt) -> dict[str, object]:
    return {
        "manifest": (
            _artifact_document(value.manifest) if value.manifest is not None else None
        ),
        "manifest_path": value.manifest_path,
        "status": value.status,
        "trial_id": value.trial_id,
    }


def _attempt_value(value: object, location: str) -> TrialAttempt:
    document = _object(value, location)
    _keys(document, {"manifest", "manifest_path", "status", "trial_id"}, location)
    manifest = document["manifest"]
    return TrialAttempt(
        trial_id=_string(document["trial_id"], f"{location}.trial_id") or "",
        status=_string(document["status"], f"{location}.status") or "",
        manifest_path=_string(
            document["manifest_path"], f"{location}.manifest_path"
        )
        or "",
        manifest=(
            None
            if manifest is None
            else _artifact_value(manifest, f"{location}.manifest")
        ),
    )


def _state_document(value: SessionState) -> dict[str, object]:
    return {
        "accepted": _config_pair_document(value.accepted),
        "attempts": [_attempt_document(attempt) for attempt in value.attempts],
        "details": value.details,
        "generation": value.generation,
        "identity": _identity_document(value.identity),
        "phase": value.phase,
        "previous_state_sha256": value.previous_state_sha256,
        "schema": value.schema,
        "session_id": value.session_id,
    }


def _state_value(
    document: dict[str, Any],
    *,
    state_path: str,
    state_sha256: str,
) -> SessionState:
    expected = {
        "accepted",
        "attempts",
        "details",
        "generation",
        "identity",
        "phase",
        "previous_state_sha256",
        "schema",
        "session_id",
    }
    _keys(document, expected, state_path)
    previous = _string(
        document["previous_state_sha256"],
        f"{state_path}.previous_state_sha256",
        nullable=True,
    )
    details = _object(document["details"], f"{state_path}.details")
    return SessionState(
        schema=_string(document["schema"], f"{state_path}.schema") or "",
        session_id=_string(document["session_id"], f"{state_path}.session_id")
        or "",
        generation=_integer(document["generation"], f"{state_path}.generation"),
        phase=_string(document["phase"], f"{state_path}.phase") or "",
        previous_state_sha256=previous,
        identity=_identity_value(document["identity"], f"{state_path}.identity"),
        accepted=_config_pair_value(document["accepted"], f"{state_path}.accepted"),
        attempts=tuple(
            _attempt_value(item, f"{state_path}.attempts[{index}]")
            for index, item in enumerate(
                _array(document["attempts"], f"{state_path}.attempts")
            )
        ),
        details=details,
        state_path=state_path,
        state_sha256=state_sha256,
    )


def _state_path(generation: int) -> str:
    return f"state/generation-{generation:08d}.json"


def _pointer_document(state: SessionState) -> dict[str, object]:
    return {
        "generation": state.generation,
        "schema": POINTER_SCHEMA,
        "session_id": state.session_id,
        "state_path": state.state_path,
        "state_sha256": state.state_sha256,
    }


def _attempt_transition_allowed(before: TrialAttempt, after: TrialAttempt) -> bool:
    if before.trial_id != after.trial_id or before.manifest_path != after.manifest_path:
        return False
    allowed = {
        "pending": {"pending", "running", "abandoned"},
        "running": {"running", "complete", "abandoned"},
        "complete": {"complete"},
        "abandoned": {"abandoned"},
    }
    if after.status not in allowed[before.status]:
        return False
    if before.status in ("complete", "abandoned") and before != after:
        return False
    return True


class TuningSessionStore:
    """Persistence boundary for one bootstrap/tuning session directory."""

    def __init__(self, root: pathlib.Path):
        self._root = root
        self._pointer = root / "session.json"
        self._state_dir = root / "state"

    def _path(self, relative: str) -> pathlib.Path:
        path = (self._root / pathlib.PurePosixPath(relative)).resolve()
        try:
            path.relative_to(self._root.resolve())
        except ValueError as error:
            raise SessionError(f"artifact path escapes session: {relative}") from error
        return path

    def _verify_artifact(self, reference: ArtifactRef) -> pathlib.Path:
        path = self._path(reference.path)
        _require(path.is_file(), f"session artifact is missing: {reference.path}")
        _require(
            path.stat().st_size == reference.size_bytes,
            f"session artifact size mismatch: {reference.path}",
        )
        _require(
            sha256_file(path) == reference.sha256,
            f"session artifact SHA-256 mismatch: {reference.path}",
        )
        return path

    def verify_artifact(self, reference: ArtifactRef) -> pathlib.Path:
        """Resolve and verify one immutable session artifact reference."""

        return self._verify_artifact(reference)

    def _verify_trial(
        self,
        attempt: TrialAttempt,
        identity: SessionIdentity,
    ) -> None:
        if attempt.status != "complete" or attempt.manifest is None:
            return
        manifest_path = self._verify_artifact(attempt.manifest)
        try:
            manifest = load_trial_manifest(
                manifest_path,
                artifact_root=manifest_path.parent,
            )
            _require(
                manifest.trial_id == attempt.trial_id,
                "trial manifest ID does not match tuning state",
            )
            _require(manifest.status == "success", "tuning trial did not succeed")
            _require(
                manifest.target_endpoint_id == identity.target.endpoint_id,
                "trial target ID does not match tuning identity",
            )
            endpoints = {
                endpoint.spec.endpoint_id: endpoint
                for endpoint in manifest.endpoints
            }
            _require(
                set(endpoints) == {
                    identity.target.endpoint_id,
                    identity.peer.endpoint_id,
                },
                "trial endpoint IDs do not match tuning identity",
            )
            for expected in (identity.target, identity.peer):
                endpoint = endpoints[expected.endpoint_id]
                _require(
                    endpoint.spec.role == expected.role,
                    f"trial role mismatch for {expected.endpoint_id}",
                )
                for field in ("git_commit", "binary_sha256", "build"):
                    _require(
                        getattr(endpoint.fingerprints, field)
                        == getattr(expected.fingerprints, field),
                        f"trial {field} mismatch for {expected.endpoint_id}",
                    )
                source_path = (
                    pathlib.PurePosixPath("configs")
                    / "source"
                    / f"{expected.endpoint_id}.toml"
                )
                source_refs = tuple(
                    artifact
                    for artifact in endpoint.artifacts
                    if pathlib.PurePosixPath(artifact.path) == source_path
                )
                _require(
                    len(source_refs) == 1,
                    f"trial source config is ambiguous for {expected.endpoint_id}",
                )
                _require(
                    endpoint.fingerprints.source_config_sha256
                    == source_refs[0].sha256,
                    f"trial source config hash mismatch for {expected.endpoint_id}",
                )
                canonical_path = (
                    pathlib.PurePosixPath("configs")
                    / "canonical"
                    / f"{expected.endpoint_id}.json"
                )
                canonical_refs = tuple(
                    artifact
                    for artifact in endpoint.artifacts
                    if pathlib.PurePosixPath(artifact.path) == canonical_path
                )
                _require(
                    len(canonical_refs) == 1,
                    f"trial canonical config is ambiguous for {expected.endpoint_id}",
                )
                _load_document(manifest_path.parent / canonical_refs[0].path)
                metrics_refs = tuple(
                    artifact
                    for artifact in endpoint.artifacts
                    if artifact.schema == identity.metrics_schema
                )
                _require(
                    len(metrics_refs) == 1,
                    f"trial metrics artifact is ambiguous for {expected.endpoint_id}",
                )
                load_axio_jsonl(
                    manifest_path.parent / metrics_refs[0].path,
                    schema=identity.metrics_schema,
                )
            _require(
                manifest.host_metrics is not None
                and manifest.host_metrics.schema == identity.host_metrics_schema,
                "trial host metrics schema does not match tuning identity",
            )
            host_metrics_path = manifest_path.parent / manifest.host_metrics.path
            host_metrics = load_metric_sample(
                host_metrics_path,
                artifact_root=manifest_path.parent,
            )
            _require(
                host_metrics.endpoint_id == identity.target.endpoint_id,
                "host metrics endpoint does not match tuning target",
            )
            _require(
                tuple(provider.name for provider in host_metrics.providers)
                == identity.providers,
                "host metrics providers do not match tuning identity",
            )
        except (ContractError, OSError, UnicodeError) as error:
            raise SessionError(
                f"trial artifact verification failed for {attempt.trial_id}: {error}"
            ) from error

    def _verify_state_artifacts(self, state: SessionState) -> None:
        for endpoint in (state.identity.target, state.identity.peer):
            self._verify_artifact(endpoint.source_config)
        self._verify_artifact(state.accepted.target)
        self._verify_artifact(state.accepted.peer)
        for attempt in state.attempts:
            self._verify_trial(attempt, state.identity)

    def _verify_state_object(self, state: SessionState) -> None:
        _require(
            _sha256_bytes(_json_bytes(_state_document(state)))
            == state.state_sha256,
            "in-memory state object hash does not match its immutable snapshot",
        )

    def _write_immutable(self, path: pathlib.Path, document: object) -> bytes:
        payload = _json_bytes(document)
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists():
            _require(not path.is_symlink(), f"immutable orphan is a symlink: {path}")
            _require(
                path.read_bytes() == payload,
                f"divergent immutable orphan refuses overwrite: {path}",
            )
            return payload
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
        )
        temporary = pathlib.Path(temporary_name)
        try:
            with os.fdopen(descriptor, "wb") as output:
                output.write(payload)
                output.flush()
                os.fsync(output.fileno())
            try:
                os.link(temporary, path)
            except FileExistsError:
                _require(
                    path.read_bytes() == payload,
                    f"divergent immutable orphan refuses overwrite: {path}",
                )
            if hasattr(os, "O_DIRECTORY"):
                directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)
        finally:
            temporary.unlink(missing_ok=True)
        return payload

    def _publish_pointer(self, state: SessionState) -> None:
        write_json_atomic(self._pointer, _pointer_document(state))

    def _new_state(
        self,
        current: SessionState,
        *,
        phase: str,
        accepted: ConfigPair,
        attempts: tuple[TrialAttempt, ...],
        details: dict[str, Any],
    ) -> SessionState:
        generation = current.generation + 1
        state_path = _state_path(generation)
        provisional = SessionState(
            schema=STATE_SCHEMA,
            session_id=current.session_id,
            generation=generation,
            phase=phase,
            previous_state_sha256=current.state_sha256,
            identity=current.identity,
            accepted=accepted,
            attempts=attempts,
            details=details,
            state_path=state_path,
            state_sha256="0" * 64,
        )
        digest = _sha256_bytes(_json_bytes(_state_document(provisional)))
        return dataclasses.replace(provisional, state_sha256=digest)

    def create(
        self,
        *,
        session_id: str,
        identity: SessionIdentity,
        accepted: ConfigPair,
        details: dict[str, Any] | None = None,
    ) -> SessionState:
        _require(not self._pointer.exists(), "tuning session already exists")
        initial_details = {} if details is None else details
        state_path = _state_path(0)
        provisional = SessionState(
            schema=STATE_SCHEMA,
            session_id=session_id,
            generation=0,
            phase="baseline",
            previous_state_sha256=None,
            identity=identity,
            accepted=accepted,
            attempts=(),
            details=initial_details,
            state_path=state_path,
            state_sha256="0" * 64,
        )
        digest = _sha256_bytes(_json_bytes(_state_document(provisional)))
        state = dataclasses.replace(provisional, state_sha256=digest)
        self._verify_state_artifacts(state)
        payload = self._write_immutable(
            self._path(state.state_path),
            _state_document(state),
        )
        _require(
            _sha256_bytes(payload) == state.state_sha256,
            "initial state hash drift",
        )
        self._publish_pointer(state)
        return self.status()

    def _load_history(self) -> tuple[SessionState, ...]:
        _require(self._pointer.is_file(), "tuning session pointer is missing")
        pointer = _load_document(self._pointer)
        location = str(self._pointer)
        _keys(
            pointer,
            {"generation", "schema", "session_id", "state_path", "state_sha256"},
            location,
        )
        _require(
            pointer["schema"] == POINTER_SCHEMA,
            "tuning pointer schema is invalid",
        )
        generation = _integer(pointer["generation"], f"{location}.generation")
        _require(generation >= 0, "tuning pointer generation is invalid")
        session_id = _string(pointer["session_id"], f"{location}.session_id") or ""
        pointer_path = _string(pointer["state_path"], f"{location}.state_path") or ""
        pointer_sha = _string(pointer["state_sha256"], f"{location}.state_sha256") or ""
        _require(
            pointer_path == _state_path(generation),
            "tuning pointer path is invalid",
        )
        _require(
            bool(SHA256_PATTERN.fullmatch(pointer_sha)),
            "pointer SHA-256 is invalid",
        )

        previous_sha: str | None = None
        previous_state: SessionState | None = None
        initial_identity: SessionIdentity | None = None
        current: SessionState | None = None
        states: list[SessionState] = []
        for index in range(generation + 1):
            relative = _state_path(index)
            path = self._path(relative)
            _require(path.is_file(), f"tuning state generation is missing: {relative}")
            try:
                payload = path.read_bytes()
            except OSError as error:
                raise SessionError(f"cannot read tuning state {relative}: {error}") from error
            digest = _sha256_bytes(payload)
            state = _state_value(
                _document_from_bytes(payload, path),
                state_path=relative,
                state_sha256=digest,
            )
            _require(state.session_id == session_id, "tuning session ID drift")
            _require(state.generation == index, "tuning state generation drift")
            _require(
                state.previous_state_sha256 == previous_sha,
                "tuning state previous hash mismatch",
            )
            if initial_identity is None:
                initial_identity = state.identity
            else:
                _require(state.identity == initial_identity, "tuning identity drift")
            if previous_state is not None:
                self._validate_persisted_transition(previous_state, state)
            self._verify_state_artifacts(state)
            states.append(state)
            previous_sha = digest
            previous_state = state
            current = state
        _require(current is not None, "tuning session has no state")
        _require(current.state_path == pointer_path, "pointer state path mismatch")
        _require(current.state_sha256 == pointer_sha, "pointer state SHA-256 mismatch")
        return tuple(states)

    def status(self) -> SessionState:
        """Return the verified current immutable session state."""

        return self._load_history()[-1]

    def history(self) -> tuple[SessionState, ...]:
        """Return the verified immutable state chain without changing the session."""

        return self._load_history()

    def _validate_attempt_update(
        self,
        before: tuple[TrialAttempt, ...],
        after: tuple[TrialAttempt, ...],
    ) -> None:
        _require(len(after) >= len(before), "trial attempts must be append-only")
        for old, new in zip(before, after):
            _require(
                _attempt_transition_allowed(old, new),
                f"trial attempt transition is invalid for {old.trial_id}",
            )
        for attempt in after[len(before) :]:
            _require(
                attempt.status in ("pending", "running"),
                "new trial attempt must be pending or running",
            )

    def _validate_persisted_transition(
        self,
        before: SessionState,
        after: SessionState,
    ) -> None:
        if after.phase != before.phase:
            _require(
                after.phase in PHASE_TRANSITIONS[before.phase],
                "persisted phase transition is illegal: "
                f"{before.phase} -> {after.phase}",
            )
        else:
            _require(
                before.phase != "complete",
                "persisted complete state must be terminal",
            )
        if after.accepted != before.accepted:
            _require(
                after.phase == "accepted" and after.phase != before.phase,
                "persisted accepted config changed outside acceptance",
            )
        self._validate_attempt_update(before.attempts, after.attempts)

    def _transition(
        self,
        current: SessionState,
        *,
        phase: str,
        accepted: ConfigPair | None,
        attempts: tuple[TrialAttempt, ...] | None,
        details: dict[str, Any] | None,
        allow_same_phase: bool,
    ) -> SessionState:
        self._verify_state_object(current)
        if allow_same_phase:
            _require(phase == current.phase, "internal transition must keep its phase")
        else:
            _require(
                phase in PHASE_TRANSITIONS[current.phase],
                f"illegal tuning phase transition {current.phase} -> {phase}",
            )
        next_accepted = current.accepted if accepted is None else accepted
        if next_accepted != current.accepted:
            _require(
                not allow_same_phase and phase == "accepted",
                "accepted config may change only on the accepted phase",
            )
        next_attempts = current.attempts if attempts is None else attempts
        self._validate_attempt_update(current.attempts, next_attempts)
        next_details = {} if details is None else details
        intended = self._new_state(
            current,
            phase=phase,
            accepted=next_accepted,
            attempts=next_attempts,
            details=next_details,
        )
        authoritative = self.status()
        if authoritative.state_sha256 != current.state_sha256:
            if (
                authoritative.generation == intended.generation
                and authoritative.state_sha256 == intended.state_sha256
            ):
                return authoritative
            raise SessionError("stale tuning state compare-and-swap refused")
        self._verify_state_artifacts(intended)
        payload = self._write_immutable(
            self._path(intended.state_path),
            _state_document(intended),
        )
        _require(
            _sha256_bytes(payload) == intended.state_sha256,
            "published tuning state hash drift",
        )
        self._publish_pointer(intended)
        return self.status()

    def transition(
        self,
        current: SessionState,
        *,
        phase: str,
        accepted: ConfigPair | None = None,
        attempts: tuple[TrialAttempt, ...] | None = None,
        details: dict[str, Any] | None = None,
    ) -> SessionState:
        return self._transition(
            current,
            phase=phase,
            accepted=accepted,
            attempts=attempts,
            details=details,
            allow_same_phase=False,
        )

    def start_trial(self, current: SessionState, trial_id: str) -> SessionState:
        _require(current.phase != "complete", "complete session cannot start a trial")
        _require(
            not any(attempt.status == "running" for attempt in current.attempts),
            "another trial is already running",
        )
        existing = next(
            (attempt for attempt in current.attempts if attempt.trial_id == trial_id),
            None,
        )
        trial_directory = self._path(f"trials/{trial_id}")
        _require(
            not trial_directory.exists(),
            f"trial directory already exists: {trial_directory}",
        )
        attempts = list(current.attempts)
        if existing is None:
            _require(
                not any(attempt.status == "pending" for attempt in attempts),
                "a different retry is pending",
            )
            attempts.append(
                TrialAttempt(
                    trial_id=trial_id,
                    status="running",
                    manifest_path=f"trials/{trial_id}/trial.json",
                    manifest=None,
                )
            )
        else:
            _require(existing.status == "pending", "trial ID cannot be started again")
            attempts[attempts.index(existing)] = dataclasses.replace(
                existing, status="running"
            )
        return self._transition(
            current,
            phase=current.phase,
            accepted=None,
            attempts=tuple(attempts),
            details=current.details,
            allow_same_phase=True,
        )

    def abandon_active_trial(
        self,
        current: SessionState,
        *,
        details: dict[str, Any] | None = None,
    ) -> SessionState:
        """Close one failed running or pending attempt without creating a retry."""

        active = tuple(
            attempt
            for attempt in current.attempts
            if attempt.status in ("pending", "running")
        )
        _require(len(active) == 1, "exactly one active trial is required")
        attempt = active[0]
        _require(
            not self._path(attempt.manifest_path).exists(),
            "published trial must be finalized instead of abandoned",
        )
        attempts = list(current.attempts)
        attempts[attempts.index(attempt)] = dataclasses.replace(
            attempt,
            status="abandoned",
        )
        return self._transition(
            current,
            phase=current.phase,
            accepted=None,
            attempts=tuple(attempts),
            details=current.details if details is None else details,
            allow_same_phase=True,
        )

    def checkpoint(
        self,
        current: SessionState,
        *,
        details: dict[str, Any],
    ) -> SessionState:
        """Persist same-phase controller progress at a completed round boundary."""

        return self._transition(
            current,
            phase=current.phase,
            accepted=None,
            attempts=current.attempts,
            details=details,
            allow_same_phase=True,
        )

    def cleanup_temporary_files(self) -> int:
        patterns = (
            (self._root, re.compile(r"^\.session\.json\..+\.tmp$")),
            (
                self._state_dir,
                re.compile(r"^\.generation-\d{8}\.json\..+\.tmp$"),
            ),
        )
        removed = 0
        for directory, pattern in patterns:
            if not directory.is_dir():
                continue
            for path in directory.iterdir():
                if (
                    pattern.fullmatch(path.name)
                    and path.is_file()
                    and not path.is_symlink()
                ):
                    path.unlink()
                    removed += 1
        return removed

    def resume(
        self,
        expected_identity: SessionIdentity,
        *,
        retry_trial_id: str | None = None,
        details: dict[str, Any] | None = None,
    ) -> RecoveryResult:
        self.cleanup_temporary_files()
        current = self.status()
        _require(
            current.identity == expected_identity,
            "tuning session identity mismatch",
        )
        pending = tuple(
            attempt for attempt in current.attempts if attempt.status == "pending"
        )
        running = tuple(
            attempt for attempt in current.attempts if attempt.status == "running"
        )
        if not running:
            _require(details is None, "recovery details require a running trial")
            if retry_trial_id is not None and pending:
                _require(
                    retry_trial_id == pending[0].trial_id,
                    "a different pending retry ID already exists",
                )
            return RecoveryResult(current, "none", None)

        attempt = running[0]
        manifest_path = self._path(attempt.manifest_path)
        attempts = list(current.attempts)
        attempt_index = attempts.index(attempt)
        if manifest_path.exists():
            _require(manifest_path.is_file(), "partial trial manifest is not a file")
            try:
                reference = artifact_ref(
                    self._root,
                    manifest_path,
                    schema="pipetune.trial/v1",
                )
                completed = dataclasses.replace(
                    attempt,
                    status="complete",
                    manifest=reference,
                )
                self._verify_trial(completed, current.identity)
            except (ContractError, OSError, SessionError) as error:
                raise SessionError(
                    f"partially published trial {attempt.trial_id} is invalid: {error}"
                ) from error
            attempts[attempt_index] = completed
            state = self._transition(
                current,
                phase=current.phase,
                accepted=None,
                attempts=tuple(attempts),
                details=current.details if details is None else details,
                allow_same_phase=True,
            )
            return RecoveryResult(state, "finalized", attempt.trial_id)

        _require(
            retry_trial_id is not None,
            "interrupted trial requires a new retry ID",
        )
        _require(
            bool(TRIAL_ID_PATTERN.fullmatch(retry_trial_id)),
            "retry trial ID is invalid",
        )
        _require(
            all(item.trial_id != retry_trial_id for item in attempts),
            "retry trial ID must be new",
        )
        retry_directory = self._path(f"trials/{retry_trial_id}")
        _require(
            not retry_directory.exists(),
            f"retry trial directory already exists: {retry_directory}",
        )
        attempts[attempt_index] = dataclasses.replace(attempt, status="abandoned")
        attempts.append(
            TrialAttempt(
                trial_id=retry_trial_id,
                status="pending",
                manifest_path=f"trials/{retry_trial_id}/trial.json",
                manifest=None,
            )
        )
        state = self._transition(
            current,
            phase=current.phase,
            accepted=None,
            attempts=tuple(attempts),
            details=current.details if details is None else details,
            allow_same_phase=True,
        )
        return RecoveryResult(state, "rerun", retry_trial_id)
