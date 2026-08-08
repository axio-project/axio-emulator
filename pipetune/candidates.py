"""Deterministic target-only candidate generation for PipeTune."""

from __future__ import annotations

import copy
import dataclasses
import hashlib
import json
import os
import pathlib
import shutil
import tempfile
from collections.abc import Callable
from typing import Any, Protocol

from pipetune.diagnosis import Diagnosis
from pipetune.runner import MeasureError
from pipetune.search_policy import (
    SearchAction,
    SearchPolicyError,
    memory_actions,
)
from pipetune.topology_state import TopologyState, TopologyStateError


class CandidateError(RuntimeError):
    """Raised when a configuration tool violates candidate invariants."""


class CandidateConfigTool(Protocol):
    def dump(self, path: pathlib.Path) -> dict[str, object]: ...

    def materialize_target_pair(
        self,
        *,
        target_input: pathlib.Path,
        peer_input: pathlib.Path,
        target_output: pathlib.Path,
        peer_output: pathlib.Path,
        overrides: dict[str, int],
    ) -> None: ...

    def materialize_target_profile_pair(
        self,
        *,
        target_input: pathlib.Path,
        peer_input: pathlib.Path,
        target_output: pathlib.Path,
        peer_output: pathlib.Path,
        profile: str,
        overrides: dict[str, int],
    ) -> None: ...

    def materialize_target(
        self,
        *,
        target_input: pathlib.Path,
        target_output: pathlib.Path,
        overrides: dict[str, int],
    ) -> None: ...

    def validate_pair(
        self, target_config: pathlib.Path, peer_config: pathlib.Path
    ) -> None: ...


CandidateAction = SearchAction


@dataclasses.dataclass(frozen=True)
class Candidate:
    candidate_id: str
    action: SearchAction
    target_config: pathlib.Path
    peer_config: pathlib.Path
    target_sha256: str
    peer_sha256: str
    canonical_target: dict[str, object]
    canonical_peer: dict[str, object]


def _runtime(config: dict[str, object]) -> dict[str, object]:
    try:
        runtime = config["knobs"]["runtime"]
    except (KeyError, TypeError) as error:
        raise CandidateError("target config has no runtime knobs") from error
    if not isinstance(runtime, dict):
        raise CandidateError("target runtime knobs must be an object")
    return runtime


def actions_for_diagnosis(
    diagnosis: Diagnosis, target: dict[str, object]
) -> tuple[SearchAction, ...]:
    """Map one diagnosis to ordered, single-action target candidates."""

    try:
        return memory_actions(
            diagnosis,
            TopologyState.from_config(target),
            _runtime(target),
        )
    except (SearchPolicyError, TopologyStateError) as error:
        raise CandidateError(str(error)) from error


def _canonical_payload(document: dict[str, object]) -> bytes:
    return json.dumps(
        document,
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def _canonical_sha(document: dict[str, object]) -> str:
    return hashlib.sha256(_canonical_payload(document)).hexdigest()


def _set_path(document: dict[str, object], path: str, value: int) -> None:
    current: Any = document
    parts = path.split(".")
    for part in parts[:-1]:
        if not isinstance(current, dict) or part not in current:
            raise CandidateError(f"config is missing {path}")
        current = current[part]
    if not isinstance(current, dict) or parts[-1] not in current:
        raise CandidateError(f"config is missing {path}")
    current[parts[-1]] = value


def _normalize_groups(document: dict[str, object], *, applications_only: bool) -> None:
    try:
        workloads = document["deployment"]["topology"]["workloads"]
    except (KeyError, TypeError) as error:
        raise CandidateError("config has no materializable topology") from error
    if not isinstance(workloads, list):
        raise CandidateError("topology workloads must be an array")
    for workload in workloads:
        if not isinstance(workload, dict):
            raise CandidateError("topology workload must be an object")
        if applications_only:
            groups = workload.get("groups")
            if not isinstance(groups, list):
                raise CandidateError("topology groups must be an array")
            for group in groups:
                if not isinstance(group, dict) or "applications" not in group:
                    raise CandidateError("topology group has no applications")
                group["applications"] = "<C1-derived>"
        else:
            workload["groups"] = "<C2-derived>"


def _normalize_remote_routes(document: dict[str, object]) -> None:
    try:
        workloads = document["deployment"]["topology"]["workloads"]
    except (KeyError, TypeError) as error:
        raise CandidateError("peer config has no topology workloads") from error
    if not isinstance(workloads, list):
        raise CandidateError("peer topology workloads must be an array")
    for workload in workloads:
        if not isinstance(workload, dict) or "remote_dispatchers" not in workload:
            raise CandidateError("peer workload has no remote_dispatchers")
        workload["remote_dispatchers"] = "<C2-reciprocal-route>"


def _require_invariants(
    action: SearchAction,
    before_target: dict[str, object],
    before_peer: dict[str, object],
    after_target: dict[str, object],
    after_peer: dict[str, object],
) -> None:
    expected_target = copy.deepcopy(before_target)
    for path, value in action.overrides:
        _set_path(expected_target, path, value)
    actual_target = copy.deepcopy(after_target)
    expected_peer = copy.deepcopy(before_peer)
    actual_peer = copy.deepcopy(after_peer)
    if action.profile is not None:
        _normalize_groups(expected_target, applications_only=False)
        _normalize_groups(actual_target, applications_only=False)
        _normalize_remote_routes(expected_peer)
        _normalize_remote_routes(actual_peer)
    elif action.kind == "c1":
        _normalize_groups(expected_target, applications_only=True)
        _normalize_groups(actual_target, applications_only=True)
    elif action.kind == "c2":
        _normalize_groups(expected_target, applications_only=False)
        _normalize_groups(actual_target, applications_only=False)
        _normalize_remote_routes(expected_peer)
        _normalize_remote_routes(actual_peer)
    if actual_target != expected_target:
        raise CandidateError(f"{action.name} changed target fields outside its action")
    if actual_peer != expected_peer:
        raise CandidateError(f"{action.name} changed frozen peer fields")


def publish_actions(
    actions: tuple[SearchAction, ...],
    *,
    target_config: pathlib.Path,
    peer_config: pathlib.Path,
    output_dir: pathlib.Path,
    config_tool: CandidateConfigTool,
    candidate_validator: Callable[[SearchAction, dict[str, object]], bool]
    | None = None,
) -> tuple[Candidate, ...]:
    """Materialize, validate, de-duplicate, and atomically publish candidates."""

    if output_dir.exists():
        raise CandidateError(f"candidate output already exists: {output_dir}")
    before_target = config_tool.dump(target_config)
    before_peer = config_tool.dump(peer_config)
    seen = {(_canonical_sha(before_target), _canonical_sha(before_peer))}
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    staging: pathlib.Path | None = pathlib.Path(
        tempfile.mkdtemp(prefix=f".{output_dir.name}.", dir=output_dir.parent)
    )
    pending: list[
        tuple[
            str,
            SearchAction,
            tuple[str, str],
            dict[str, object],
            dict[str, object],
        ]
    ] = []
    try:
        for action_index, action in enumerate(actions):
            candidate_id = f"candidate-{action_index + 1:02d}-{action.name}"
            candidate_root = staging / candidate_id
            candidate_root.mkdir()
            target_output = candidate_root / "target.toml"
            peer_output = candidate_root / "peer.toml"
            overrides = dict(action.overrides)
            try:
                if action.profile is not None:
                    config_tool.materialize_target_profile_pair(
                        target_input=target_config,
                        peer_input=peer_config,
                        target_output=target_output,
                        peer_output=peer_output,
                        profile=action.profile,
                        overrides=overrides,
                    )
                elif action.kind in ("c1", "c2"):
                    config_tool.materialize_target_pair(
                        target_input=target_config,
                        peer_input=peer_config,
                        target_output=target_output,
                        peer_output=peer_output,
                        overrides=overrides,
                    )
                else:
                    config_tool.materialize_target(
                        target_input=target_config,
                        target_output=target_output,
                        overrides=overrides,
                    )
                    shutil.copyfile(peer_config, peer_output)
                config_tool.validate_pair(target_output, peer_output)
                canonical_target = config_tool.dump(target_output)
                canonical_peer = config_tool.dump(peer_output)
            except MeasureError:
                shutil.rmtree(candidate_root)
                continue
            _require_invariants(
                action,
                before_target,
                before_peer,
                canonical_target,
                canonical_peer,
            )
            if candidate_validator is not None and not candidate_validator(
                action, canonical_target
            ):
                shutil.rmtree(candidate_root)
                continue
            pair_hash = (
                _canonical_sha(canonical_target),
                _canonical_sha(canonical_peer),
            )
            if pair_hash in seen:
                shutil.rmtree(candidate_root)
                continue
            seen.add(pair_hash)
            pending.append(
                (
                    candidate_id,
                    action,
                    pair_hash,
                    canonical_target,
                    canonical_peer,
                )
            )
        os.replace(staging, output_dir)
        staging = output_dir
        if hasattr(os, "O_DIRECTORY"):
            descriptor = os.open(output_dir.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
        candidates = tuple(
            Candidate(
                candidate_id=candidate_id,
                action=action,
                target_config=output_dir / candidate_id / "target.toml",
                peer_config=output_dir / candidate_id / "peer.toml",
                target_sha256=pair_hash[0],
                peer_sha256=pair_hash[1],
                canonical_target=canonical_target,
                canonical_peer=canonical_peer,
            )
            for candidate_id, action, pair_hash, canonical_target, canonical_peer in pending
        )
        staging = None
        return candidates
    finally:
        if staging is not None:
            shutil.rmtree(staging, ignore_errors=True)


def generate_candidates(
    diagnosis: Diagnosis,
    *,
    target_config: pathlib.Path,
    peer_config: pathlib.Path,
    output_dir: pathlib.Path,
    config_tool: CandidateConfigTool,
) -> tuple[Candidate, ...]:
    """Materialize the complete paper-derived candidate set."""

    from pipetune.topology_candidates import materialize_actions

    before_target = config_tool.dump(target_config)
    actions = actions_for_diagnosis(diagnosis, before_target)
    return materialize_actions(
        actions,
        target_config=target_config,
        peer_config=peer_config,
        output_dir=output_dir,
        config_tool=config_tool,
    )


def generate_lock_averse_candidates(
    diagnosis: Diagnosis,
    *,
    target_config: pathlib.Path,
    peer_config: pathlib.Path,
    output_dir: pathlib.Path,
    config_tool: CandidateConfigTool,
) -> tuple[Candidate, ...]:
    """Compatibility entry point for the topology-aware memory policy."""

    return generate_candidates(
        diagnosis,
        target_config=target_config,
        peer_config=peer_config,
        output_dir=output_dir,
        config_tool=config_tool,
    )


__all__ = [
    "Candidate",
    "CandidateAction",
    "CandidateConfigTool",
    "CandidateError",
    "actions_for_diagnosis",
    "generate_candidates",
    "generate_lock_averse_candidates",
    "publish_actions",
]
