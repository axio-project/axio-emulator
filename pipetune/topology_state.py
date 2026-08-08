"""Canonical active-topology state for PipeTune search decisions."""

from __future__ import annotations

import dataclasses
from collections.abc import Mapping
from types import MappingProxyType


class TopologyStateError(RuntimeError):
    """Raised when a canonical topology cannot support PipeTune search."""


@dataclasses.dataclass(frozen=True)
class TopologyState:
    """The active application/dispatcher placement in a canonical config."""

    application_ids: tuple[int, ...]
    dispatcher_ids: tuple[int, ...]
    overlap_count: int
    physical_core_count: int
    physical_core_budget: int
    fanout_by_dispatcher: Mapping[int, int]
    colocated_dispatcher_count: int

    @classmethod
    def from_config(cls, document: dict[str, object]) -> "TopologyState":
        topology = _topology_document(document)
        return _state_from_topology(document, topology)

    @property
    def application_count(self) -> int:
        return len(self.application_ids)

    @property
    def dispatcher_count(self) -> int:
        return len(self.dispatcher_ids)

    @property
    def balanced_fanout(self) -> bool:
        values = tuple(self.fanout_by_dispatcher.values())
        return bool(values) and max(values) == min(values)


def _object(value: object, path: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise TopologyStateError(f"{path} must be an object")
    return value


def _array(value: object, path: str) -> list[object]:
    if not isinstance(value, list):
        raise TopologyStateError(f"{path} must be an array")
    return value


def _integer(value: object, path: str) -> int:
    if type(value) is not int:
        raise TopologyStateError(f"{path} must be an integer")
    return value


def _ids(value: object, path: str) -> tuple[int, ...]:
    ids = tuple(_integer(item, path) for item in _array(value, path))
    if len(ids) != len(set(ids)):
        raise TopologyStateError(f"{path} must not contain duplicate IDs")
    return ids


def _topology_document(document: dict[str, object]) -> dict[str, object]:
    deployment = _object(document.get("deployment"), "deployment")
    return _object(deployment.get("topology"), "deployment.topology")


def _runtime(document: dict[str, object]) -> dict[str, object]:
    knobs = _object(document.get("knobs"), "knobs")
    return _object(knobs.get("runtime"), "knobs.runtime")


def _workspace_ids(topology: dict[str, object]) -> tuple[int, ...]:
    ids: list[int] = []
    for index, value in enumerate(
        _array(topology.get("workspaces"), "deployment.topology.workspaces")
    ):
        workspace = _object(value, f"deployment.topology.workspaces[{index}]")
        ids.append(
            _integer(
                workspace.get("id"), f"deployment.topology.workspaces[{index}].id"
            )
        )
    if len(ids) != len(set(ids)):
        raise TopologyStateError("deployment.topology.workspaces has duplicate IDs")
    return tuple(ids)


def _active_roles(
    topology: dict[str, object],
) -> tuple[tuple[int, ...], tuple[int, ...], dict[int, int], set[int]]:
    applications: list[int] = []
    dispatchers: list[int] = []
    fanout: dict[int, int] = {}
    colocated_dispatchers: set[int] = set()
    workloads = _array(topology.get("workloads"), "deployment.topology.workloads")
    for workload_index, value in enumerate(workloads):
        workload_path = f"deployment.topology.workloads[{workload_index}]"
        workload = _object(value, workload_path)
        for group_index, group_value in enumerate(
            _array(workload.get("groups"), f"{workload_path}.groups")
        ):
            group_path = f"{workload_path}.groups[{group_index}]"
            group = _object(group_value, group_path)
            dispatcher = _integer(group.get("dispatcher"), f"{group_path}.dispatcher")
            group_applications = _ids(
                group.get("applications"), f"{group_path}.applications"
            )
            dispatchers.append(dispatcher)
            applications.extend(group_applications)
            fanout[dispatcher] = fanout.get(dispatcher, 0) + len(group_applications)
            if dispatcher in group_applications:
                colocated_dispatchers.add(dispatcher)
    if len(applications) != len(set(applications)):
        raise TopologyStateError("active applications have duplicate ownership")
    application_ids = tuple(applications)
    dispatcher_ids = tuple(dict.fromkeys(dispatchers))
    if not application_ids or not dispatcher_ids:
        raise TopologyStateError("topology must have active applications and dispatchers")
    return application_ids, dispatcher_ids, fanout, colocated_dispatchers


def _state_from_topology(
    document: dict[str, object], topology: dict[str, object]
) -> TopologyState:
    application_pool = _ids(
        topology.get("application_workspaces"),
        "deployment.topology.application_workspaces",
    )
    dispatcher_pool = _ids(
        topology.get("dispatcher_workspaces"),
        "deployment.topology.dispatcher_workspaces",
    )
    workspace_ids = _workspace_ids(topology)
    application_ids, dispatcher_ids, fanout, colocated_dispatchers = _active_roles(
        topology
    )

    if not set(application_ids).issubset(application_pool):
        raise TopologyStateError("active application ID is outside its role pool")
    if not set(dispatcher_ids).issubset(dispatcher_pool):
        raise TopologyStateError("active dispatcher ID is outside its role pool")
    active_ids = set(application_ids) | set(dispatcher_ids)
    workspace_id_set = set(workspace_ids)
    if not set(application_pool).issubset(workspace_id_set):
        raise TopologyStateError("application role pool references an undefined workspace ID")
    if not set(dispatcher_pool).issubset(workspace_id_set):
        raise TopologyStateError("dispatcher role pool references an undefined workspace ID")
    if not active_ids.issubset(workspace_id_set):
        raise TopologyStateError("active role references an undefined workspace ID")
    runtime = _runtime(document)
    application_count = _integer(
        runtime.get("application_core_count"),
        "knobs.runtime.application_core_count",
    )
    dispatcher_count = _integer(
        runtime.get("dispatcher_queue_count"),
        "knobs.runtime.dispatcher_queue_count",
    )
    if application_count != len(application_ids):
        raise TopologyStateError("C1 does not match the active application count")
    if dispatcher_count != len(dispatcher_ids):
        raise TopologyStateError("C2 does not match the active dispatcher count")

    physical_core_count = len(active_ids)
    physical_core_budget = len(workspace_ids)
    if physical_core_count > physical_core_budget:
        raise TopologyStateError("active topology exceeds its physical core budget")
    return TopologyState(
        application_ids=application_ids,
        dispatcher_ids=dispatcher_ids,
        overlap_count=len(set(application_ids) & set(dispatcher_ids)),
        physical_core_count=physical_core_count,
        physical_core_budget=physical_core_budget,
        fanout_by_dispatcher=MappingProxyType(dict(fanout)),
        colocated_dispatcher_count=len(colocated_dispatchers),
    )
