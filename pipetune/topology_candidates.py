"""Materialize and verify topology-aware PipeTune search actions."""

from __future__ import annotations

import pathlib

from pipetune.candidates import (
    Candidate,
    CandidateConfigTool,
    CandidateError,
    publish_actions,
)
from pipetune.search_policy import SearchAction
from pipetune.topology_state import TopologyState, TopologyStateError


def _groups(document: dict[str, object]) -> tuple[dict[str, object], ...]:
    try:
        workloads = document["deployment"]["topology"]["workloads"]
    except (KeyError, TypeError) as error:
        raise CandidateError("candidate has no topology workloads") from error
    if not isinstance(workloads, list):
        raise CandidateError("candidate topology workloads must be an array")
    groups: list[dict[str, object]] = []
    for workload in workloads:
        if not isinstance(workload, dict) or not isinstance(workload.get("groups"), list):
            raise CandidateError("candidate workload groups must be an array")
        for group in workload["groups"]:
            if not isinstance(group, dict):
                raise CandidateError("candidate topology group must be an object")
            groups.append(group)
    return tuple(groups)


def _require_profile(
    action: SearchAction, document: dict[str, object], state: TopologyState
) -> None:
    if action.profile is None:
        return
    groups = _groups(document)
    one_to_one = (
        state.application_count == state.dispatcher_count
        and set(state.fanout_by_dispatcher.values()) == {1}
    )
    if action.profile == "split-1to1":
        valid = one_to_one and state.overlap_count == 0
    elif action.profile == "colocated-1to1":
        valid = (
            one_to_one
            and state.overlap_count == state.application_count
            and all(group.get("applications") == [group.get("dispatcher")] for group in groups)
        )
    elif action.profile == "colocated-fanout":
        fanouts = set(state.fanout_by_dispatcher.values())
        valid = (
            state.balanced_fanout
            and state.overlap_count == state.dispatcher_count
            and len(fanouts) == 1
            and next(iter(fanouts)) >= 2
            and all(
                isinstance(group.get("applications"), list)
                and group.get("dispatcher") in group["applications"]
                for group in groups
            )
        )
    else:
        raise CandidateError(f"unsupported topology profile {action.profile!r}")
    if not valid:
        raise CandidateError(
            f"{action.name} did not materialize profile {action.profile}"
        )


def _validate_action(action: SearchAction, document: dict[str, object]) -> bool:
    try:
        state = TopologyState.from_config(document)
    except TopologyStateError as error:
        raise CandidateError(f"{action.name} has invalid topology: {error}") from error
    _require_profile(action, document, state)
    return state.balanced_fanout


def materialize_actions(
    actions: tuple[SearchAction, ...],
    *,
    target_config: pathlib.Path,
    peer_config: pathlib.Path,
    output_dir: pathlib.Path,
    config_tool: CandidateConfigTool,
) -> tuple[Candidate, ...]:
    """Atomically publish canonical, topology-validated action candidates."""

    return publish_actions(
        actions,
        target_config=target_config,
        peer_config=peer_config,
        output_dir=output_dir,
        config_tool=config_tool,
        candidate_validator=_validate_action,
    )


__all__ = ["materialize_actions"]
