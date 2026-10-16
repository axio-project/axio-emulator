"""One resumable memory/compute policy; placement is data, not an algorithm."""
from __future__ import annotations

import dataclasses
import math

from pipetune.search_policy import (
    C1, C2, SearchPhase, local_placement_actions, memory_actions, placement,
)


def llc_rate(summary, direction):
    statistic = summary.counters.get("llc_load" if direction == "rx" else "llc_store")
    rate = statistic.median if statistic is not None else None
    return rate if rate is not None and math.isfinite(rate) and 0 <= rate <= 100 else None


@dataclasses.dataclass(frozen=True)
class SearchState:
    phase: str = "memory"
    direction: str | None = None
    application_count: int | None = None
    dispatcher_count: int | None = None
    accelerate: bool = False
    local_count: int | None = None
    local_exhausted: bool = False
    exploratory: bool = False

    def to_document(self):
        return dataclasses.asdict(self)

    @classmethod
    def from_document(cls, value):
        if not isinstance(value, dict) or set(value) != {f.name for f in dataclasses.fields(cls)}:
            raise ValueError("search state has invalid fields")
        state = cls(**value)
        if state.phase not in ("memory", "compute") or state.direction not in (None, "rx", "tx"):
            raise ValueError("search state has invalid phase/direction")
        for name in ("application_count", "dispatcher_count", "local_count"):
            count = getattr(state, name)
            if count is not None and (type(count) is not int or count < 1):
                raise ValueError(f"search state {name} must be a positive count")
        if (state.application_count is None) != (state.dispatcher_count is None):
            raise ValueError("search cursor counts must be specified together")
        for name in ("accelerate", "local_exhausted", "exploratory"):
            if type(getattr(state, name)) is not bool:
                raise ValueError(f"search state {name} must be boolean")
        if state.local_count is not None and (
            state.application_count != state.dispatcher_count
            or state.application_count is None
            or state.local_count not in (state.application_count, state.application_count - 1)
            or state.local_exhausted
        ):
            raise ValueError("local comparison does not match the search cursor")
        return state

    def at_cursor(self, direction, topology, rate):
        counts = (topology.application_count, topology.dispatcher_count)
        if self.application_count is not None and counts != (self.application_count, self.dispatcher_count):
            raise ValueError("search cursor differs from accepted topology")
        state = self if self.direction == direction else SearchState(direction=direction)
        state = dataclasses.replace(state, application_count=counts[0], dispatcher_count=counts[1])
        if rate is None or rate > 40:
            return dataclasses.replace(state, phase="memory", local_count=None, local_exhausted=False)
        if (not state.local_exhausted and state.local_count is None
                and placement(topology) in ("colocated-1to1", "split-1to1")):
            state = dataclasses.replace(state, local_count=counts[0])
        return state

    def actions(self, diagnosis, topology, runtime):
        actions = memory_actions(diagnosis, topology, runtime)
        if self.local_count is not None:
            # P1--P4 still supplies the C3 intentions; count/layout now uses E2E.
            return (*local_placement_actions(direction=self.direction, topology=topology,
                                            count=self.local_count),
                    *(a for a in actions if a.kind == "c3"))
        if self.local_exhausted:
            return tuple(a for a in actions if a.kind == "c3")
        if not self.accelerate:
            return actions
        scaled = []
        for action in actions:
            values = dict(action.overrides)
            if action.kind == "topology" and values.get(C2, topology.dispatcher_count) < topology.dispatcher_count:
                count = (1 + topology.dispatcher_count) // 2
                fanout = topology.application_count // topology.dispatcher_count
                values = {C1: count * fanout, C2: count}
                action = dataclasses.replace(action, overrides=tuple(sorted(values.items())))
            elif action.kind == "topology" and values.get(C1, topology.application_count) < topology.application_count:
                layers = topology.application_count // topology.dispatcher_count
                values[C1] = ((1 + layers) // 2) * topology.dispatcher_count
                action = dataclasses.replace(action, overrides=tuple(sorted(values.items())))
            scaled.append(action)
        return tuple(scaled)

    def after_selection(self, topology, rate, *, objective_accepted, local=False, compute=False):
        state = dataclasses.replace(
            self, phase="memory", application_count=topology.application_count,
            dispatcher_count=topology.dispatcher_count, local_count=None,
            local_exhausted=False, accelerate=not objective_accepted,
            exploratory=not objective_accepted)
        if compute:
            return dataclasses.replace(state, accelerate=False)
        if local:
            return state.after_local_comparison(topology, accepted=True)
        return state

    def after_local_comparison(self, topology, *, accepted):
        n = topology.application_count
        # First compare the current N; next compare N-1 in both layouts.
        if accepted or self.local_count == n:
            return dataclasses.replace(self, local_count=n - 1 if n > 1 else None,
                                       local_exhausted=n == 1, exploratory=False)
        return dataclasses.replace(self, local_count=None, local_exhausted=True)

    def compute_eligible(self, rate):
        return rate is not None and rate <= 40 and self.local_count is None

    def exploratory_candidate(self, evaluations, baseline_rate):
        if baseline_rate is None or baseline_rate <= 40:
            return None
        for item in evaluations:
            t = item.candidate_topology
            if (item.trial.objective.status == "valid"
                    and llc_rate(item.trial.summary, self.direction) is not None
                    and t.application_count <= self.application_count
                    and t.dispatcher_count <= self.dispatcher_count
                    and (t.application_count, t.dispatcher_count) != (self.application_count, self.dispatcher_count)):
                return item
        return None
