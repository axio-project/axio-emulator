"""Phase-aware, controller-independent PipeTune search actions."""

from __future__ import annotations

import dataclasses
import enum
from collections.abc import Mapping
from typing import Protocol

from pipetune.topology_state import TopologyState


class SearchPolicyError(RuntimeError):
    """Raised when diagnosis or runtime state cannot form a search action."""


class SearchPhase(str, enum.Enum):
    MEMORY = "memory"
    COMPUTE = "compute"


@dataclasses.dataclass(frozen=True)
class ImpactSpec:
    kind: str
    metric: str | None
    direction: str | None


@dataclasses.dataclass(frozen=True)
class SearchAction:
    name: str
    kind: str
    overrides: tuple[tuple[str, int], ...]
    profile: str | None = None
    phase: SearchPhase = SearchPhase.MEMORY
    impact: ImpactSpec = ImpactSpec("diagnosis", None, None)
    allow_equivalent_resource_reduction: bool = False


@dataclasses.dataclass(frozen=True)
class ComputeBottleneck:
    role: str
    metric: str

    def __post_init__(self) -> None:
        if self.role not in ("application", "dispatcher"):
            raise SearchPolicyError("compute bottleneck role must be application or dispatcher")
        prefix = "app_" if self.role == "application" else "dispatcher_"
        if not self.metric.startswith(prefix) or not self.metric.endswith(".completion"):
            raise SearchPolicyError(
                f"{self.role} compute bottleneck must name a completion metric"
            )

    @classmethod
    def application(cls, metric: str) -> "ComputeBottleneck":
        return cls("application", metric)

    @classmethod
    def dispatcher(cls, metric: str) -> "ComputeBottleneck":
        return cls("dispatcher", metric)

    @property
    def direction(self) -> str:
        return "rx" if "_rx." in self.metric else "tx"


class DiagnosisLike(Protocol):
    point: str
    direction: str | None
    required_probe: object | None


class SummaryLike(Protocol):
    target: object


C1 = "knobs.runtime.application_core_count"
C2 = "knobs.runtime.dispatcher_queue_count"
C3_FIELDS = {
    "tx": (
        "knobs.runtime.app_tx_batch_size",
        "knobs.runtime.dispatcher_tx_batch_size",
        "knobs.runtime.nic_tx_post_size",
    ),
    "rx": (
        "knobs.runtime.app_rx_batch_size",
        "knobs.runtime.dispatcher_rx_batch_size",
        "knobs.runtime.nic_rx_post_size",
    ),
}


def _integer(runtime: Mapping[str, object], name: str) -> int:
    value = runtime.get(name)
    if type(value) is not int:
        raise SearchPolicyError(f"target runtime knob {name} must be an integer")
    return value


def _action(
    name: str,
    kind: str,
    values: dict[str, int],
    *,
    phase: SearchPhase,
    impact: ImpactSpec,
    profile: str | None = None,
    allow_equivalent_resource_reduction: bool = False,
) -> SearchAction:
    return SearchAction(
        name=name,
        kind=kind,
        overrides=tuple(sorted(values.items())),
        profile=profile,
        phase=phase,
        impact=impact,
        allow_equivalent_resource_reduction=allow_equivalent_resource_reduction,
    )


def _directional_c3_action(
    runtime: Mapping[str, object],
    *,
    direction: str,
    increase: bool,
    phase: SearchPhase,
    impact: ImpactSpec,
) -> SearchAction | None:
    if direction not in C3_FIELDS:
        raise SearchPolicyError("directional diagnosis must be tx or rx")
    values: dict[str, int] = {}
    for path in C3_FIELDS[direction]:
        name = path.rsplit(".", 1)[1]
        current = _integer(runtime, name)
        if not increase and (current <= 1 or current % 2 != 0):
            return None
        values[path] = current * 2 if increase else current // 2
    suffix = "increase" if increase else "decrease"
    return _action(
        f"c3-{direction}-{suffix}",
        "c3",
        values,
        phase=phase,
        impact=impact,
    )


def memory_actions(
    diagnosis: DiagnosisLike,
    topology: TopologyState,
    runtime: Mapping[str, object],
) -> tuple[SearchAction, ...]:
    """Return the ordered paper-aligned memory-search actions."""

    c1 = _integer(runtime, "application_core_count")
    c2 = _integer(runtime, "dispatcher_queue_count")
    if (c1, c2) != (topology.application_count, topology.dispatcher_count):
        raise SearchPolicyError("runtime counts do not match the canonical topology")
    if diagnosis.point == "paired_reduction_required":
        if c1 <= 1 or not _fully_colocated(topology) or c1 != c2:
            return ()
        return (
            paired_count_action(
                direction=diagnosis.direction or "",
                topology=topology,
                runtime=runtime,
                candidate_count=c1 - 1,
            ),
        )
    impact = ImpactSpec("diagnosis", diagnosis.point, diagnosis.direction)
    if diagnosis.point == "probe_required":
        probe = diagnosis.required_probe
        if (
            probe is None
            or getattr(probe, "knob", None) != C1
            or getattr(probe, "baseline_value", None) != c1
            or getattr(probe, "direction", None) not in (-1, 1)
            or getattr(probe, "candidate_value", None)
            != getattr(probe, "baseline_value", 0) + getattr(probe, "direction", 0)
        ):
            raise SearchPolicyError("diagnosis has an invalid required C1 probe")
        return (
            _action(
                "c1-probe",
                "c1",
                {C1: probe.candidate_value},
                phase=SearchPhase.MEMORY,
                impact=impact,
                allow_equivalent_resource_reduction=probe.direction < 0,
            ),
        )

    actions: list[SearchAction | None]
    if diagnosis.point == "P1":
        actions = [
            _action(
                "c1-decrease",
                "c1",
                {C1: c1 - 1},
                phase=SearchPhase.MEMORY,
                impact=impact,
                allow_equivalent_resource_reduction=True,
            ),
            _directional_c3_action(
                runtime,
                direction=diagnosis.direction or "",
                increase=True,
                phase=SearchPhase.MEMORY,
                impact=impact,
            ),
        ]
    elif diagnosis.point == "P2":
        actions = [
            _action(
                "c1-decrease",
                "c1",
                {C1: c1 - 1},
                phase=SearchPhase.MEMORY,
                impact=impact,
                allow_equivalent_resource_reduction=True,
            )
        ]
    elif diagnosis.point == "P3":
        actions = [
            _action(
                "c2-decrease",
                "c2",
                {C2: c2 - 1},
                phase=SearchPhase.MEMORY,
                impact=impact,
                allow_equivalent_resource_reduction=True,
            )
        ]
    elif diagnosis.point == "P4":
        actions = [
            _action(
                "c2-decrease",
                "c2",
                {C2: c2 - 1},
                phase=SearchPhase.MEMORY,
                impact=impact,
                allow_equivalent_resource_reduction=True,
            ),
            _directional_c3_action(
                runtime,
                direction=diagnosis.direction or "",
                increase=False,
                phase=SearchPhase.MEMORY,
                impact=impact,
            ),
        ]
    else:
        actions = []
    return tuple(action for action in actions if action is not None)


def _one_to_one(topology: TopologyState) -> bool:
    return (
        topology.application_count == topology.dispatcher_count
        and set(topology.fanout_by_dispatcher.values()) == {1}
    )


def _fully_colocated(topology: TopologyState) -> bool:
    return (
        _one_to_one(topology)
        and topology.overlap_count == topology.application_count
        and topology.colocated_dispatcher_count == topology.dispatcher_count
    )


def _fully_split(topology: TopologyState) -> bool:
    return _one_to_one(topology) and topology.overlap_count == 0


def paired_count_action(
    *,
    direction: str,
    topology: TopologyState,
    runtime: Mapping[str, object],
    candidate_count: int,
) -> SearchAction:
    """Build one direct paired-count probe from a colocated one-to-one state."""

    current_a = _integer(runtime, "application_core_count")
    current_d = _integer(runtime, "dispatcher_queue_count")
    if direction not in ("rx", "tx"):
        raise SearchPolicyError("paired reduction direction must be rx or tx")
    if (
        not _fully_colocated(topology)
        or current_a != current_d
        or (current_a, current_d)
        != (topology.application_count, topology.dispatcher_count)
    ):
        raise SearchPolicyError("paired count probe needs a colocated one-to-one state")
    if (
        type(candidate_count) is not int
        or candidate_count < 1
        or candidate_count > topology.physical_core_budget
        or candidate_count == current_a
    ):
        raise SearchPolicyError("paired count probe is outside the workspace budget")
    counter = "llc_load" if direction == "rx" else "llc_store"
    name = (
        "paired-colocated-decrease"
        if candidate_count == current_a - 1
        else f"paired-colocated-probe-{candidate_count}"
    )
    return _action(
        name,
        "topology",
        {C1: candidate_count, C2: candidate_count},
        profile="colocated-1to1",
        phase=SearchPhase.MEMORY,
        impact=ImpactSpec("counter", counter, direction),
        allow_equivalent_resource_reduction=candidate_count < current_a,
    )


def _topology_action(
    name: str,
    profile: str,
    values: dict[str, int],
    impact: ImpactSpec,
) -> SearchAction:
    return _action(
        name,
        "topology",
        values,
        profile=profile,
        phase=SearchPhase.COMPUTE,
        impact=impact,
    )


def _split_actions(
    topology: TopologyState, impact: ImpactSpec
) -> list[SearchAction]:
    application_count = topology.application_count
    dispatcher_count = topology.dispatcher_count
    budget = topology.physical_core_budget
    actions: list[SearchAction] = []
    split_impact = ImpactSpec(
        "pipeline_stall",
        "pipeline_stall",
        impact.direction,
    )
    full_split_emitted = False
    if (
        _one_to_one(topology)
        and not _fully_split(topology)
        and 2 * application_count <= budget
    ):
        actions.append(
            _topology_action("split-1to1", "split-1to1", {}, split_impact)
        )
        full_split_emitted = True

    boundary = min(application_count, dispatcher_count, budget // 2)
    boundary_is_current = (
        boundary == application_count == dispatcher_count and _fully_split(topology)
    )
    boundary_duplicates_full_split = (
        full_split_emitted
        and boundary == application_count == dispatcher_count
    )
    if boundary > 0 and not boundary_is_current and not boundary_duplicates_full_split:
        actions.append(
            _topology_action(
                "boundary-split",
                "split-1to1",
                {C1: boundary, C2: boundary},
                split_impact,
            )
        )
    return actions


def compute_actions(
    bottleneck: ComputeBottleneck,
    topology: TopologyState,
    runtime: Mapping[str, object],
) -> tuple[SearchAction, ...]:
    """Return legal compute-expansion actions within the NUMA core budget."""

    c1 = _integer(runtime, "application_core_count")
    c2 = _integer(runtime, "dispatcher_queue_count")
    if (c1, c2) != (topology.application_count, topology.dispatcher_count):
        raise SearchPolicyError("runtime counts do not match the canonical topology")
    impact = ImpactSpec("component", bottleneck.metric, bottleneck.direction)
    actions = _split_actions(topology, impact)

    if bottleneck.role == "application":
        if (
            _one_to_one(topology)
            and c1 + c2 <= topology.physical_core_budget
        ):
            actions.append(
                _topology_action(
                    "app-fanout-layer",
                    "colocated-fanout",
                    {C1: c1 + c2},
                    impact,
                )
            )
        return tuple(actions)

    if _fully_colocated(topology) and c1 + 1 <= topology.physical_core_budget:
        actions.append(
            _topology_action(
                "paired-colocated-growth",
                "colocated-1to1",
                {C1: c1 + 1, C2: c2 + 1},
                impact,
            )
        )
    elif _fully_split(topology) and 2 * (c1 + 1) <= topology.physical_core_budget:
        actions.append(
            _topology_action(
                "paired-split-growth",
                "split-1to1",
                {C1: c1 + 1, C2: c2 + 1},
                impact,
            )
        )
    dispatcher_batch = f"dispatcher_{bottleneck.direction}_batch_size"
    actions.append(
        _action(
            f"dispatcher-c3-{bottleneck.direction}-increase",
            "c3",
            {f"knobs.runtime.{dispatcher_batch}": _integer(runtime, dispatcher_batch) * 2},
            phase=SearchPhase.COMPUTE,
            impact=impact,
        )
    )
    return tuple(actions)


def detect_compute_bottleneck(summary: SummaryLike) -> ComputeBottleneck | None:
    """Return positive app/dispatcher completion evidence, or no transition."""

    component = getattr(summary.target, "dominant_component", None)
    if component is None or getattr(component, "kind", None) != "completion":
        return None
    stage = getattr(component, "stage", "")
    name = getattr(component, "name", "")
    if stage in ("app_rx", "app_tx"):
        return ComputeBottleneck.application(name)
    if stage in ("dispatcher_rx", "dispatcher_tx"):
        return ComputeBottleneck.dispatcher(name)
    return None


__all__ = [
    "C1",
    "C2",
    "C3_FIELDS",
    "ComputeBottleneck",
    "ImpactSpec",
    "SearchAction",
    "SearchPhase",
    "SearchPolicyError",
    "compute_actions",
    "detect_compute_bottleneck",
    "memory_actions",
    "paired_count_action",
]
