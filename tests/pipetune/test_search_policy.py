from __future__ import annotations

import types
import unittest

from pipetune.diagnosis import ProbeSpec
from pipetune.search_policy import (
    ComputeBottleneck,
    ImpactSpec,
    SearchPhase,
    compute_actions,
    detect_compute_bottleneck,
    memory_actions,
    paired_count_action,
)
from pipetune.topology_state import TopologyState


def _config(
    *,
    application_count: int,
    dispatcher_count: int,
    budget: int,
    profile: str,
) -> dict[str, object]:
    if profile == "colocated-1to1":
        groups = [
            {"dispatcher": index, "applications": [index]}
            for index in range(dispatcher_count)
        ]
    elif profile == "split-1to1":
        groups = [
            {
                "dispatcher": application_count + index,
                "applications": [index],
            }
            for index in range(dispatcher_count)
        ]
    elif profile == "colocated-fanout":
        groups = [
            {
                "dispatcher": index,
                "applications": list(range(index, application_count, dispatcher_count)),
            }
            for index in range(dispatcher_count)
        ]
    else:
        raise AssertionError(profile)
    return {
        "deployment": {
            "topology": {
                "application_workspaces": list(range(budget)),
                "dispatcher_workspaces": list(range(budget)),
                "workloads": [{"id": 1, "groups": groups}],
                "workspaces": [
                    {"id": index, "cpu_core": index} for index in range(budget)
                ],
            }
        },
        "knobs": {
            "runtime": {
                "application_core_count": application_count,
                "dispatcher_queue_count": dispatcher_count,
                "app_rx_batch_size": 16,
                "app_tx_batch_size": 16,
                "dispatcher_rx_batch_size": 16,
                "dispatcher_tx_batch_size": 16,
                "nic_rx_post_size": 32,
                "nic_tx_post_size": 16,
            }
        },
    }


def _diagnosis(
    point: str,
    *,
    direction: str = "rx",
    required_probe: ProbeSpec | None = None,
) -> types.SimpleNamespace:
    return types.SimpleNamespace(
        point=point,
        direction=direction,
        required_probe=required_probe,
    )


def _names(actions: tuple[object, ...]) -> tuple[str, ...]:
    return tuple(action.name for action in actions)


class MemorySearchPolicyTest(unittest.TestCase):
    def setUp(self) -> None:
        self.document = _config(
            application_count=8,
            dispatcher_count=8,
            budget=16,
            profile="colocated-1to1",
        )
        self.state = TopologyState.from_config(self.document)
        self.runtime = self.document["knobs"]["runtime"]

    def test_exact_paper_aligned_memory_table(self) -> None:
        cases = (
            ("P1", ("c1-decrease", "c3-rx-increase")),
            ("P2", ("c1-decrease",)),
            ("P3", ("c2-decrease",)),
            ("P4", ("c1-increase", "c2-decrease", "c3-rx-decrease")),
        )
        for point, expected in cases:
            with self.subTest(point=point):
                actions = memory_actions(
                    _diagnosis(point), self.state, self.runtime
                )
                self.assertEqual(_names(actions), expected)
                self.assertTrue(
                    all(action.phase is SearchPhase.MEMORY for action in actions)
                )

    def test_preserves_the_required_c1_probe(self) -> None:
        probe = ProbeSpec(
            knob="knobs.runtime.application_core_count",
            direction=-1,
            baseline_value=8,
            candidate_value=7,
        )
        actions = memory_actions(
            _diagnosis("probe_required", required_probe=probe),
            self.state,
            self.runtime,
        )
        self.assertEqual(_names(actions), ("c1-probe",))
        self.assertEqual(dict(actions[0].overrides), {probe.knob: 7})

    def test_count_reductions_allow_equivalent_resource_savings(self) -> None:
        for point in ("P1", "P2", "P3", "P4"):
            with self.subTest(point=point):
                actions = memory_actions(
                    _diagnosis(point), self.state, self.runtime
                )
                for action in actions:
                    self.assertEqual(
                        action.allow_equivalent_resource_reduction,
                        action.name in ("c1-decrease", "c2-decrease"),
                    )

    def test_fully_colocated_pair_decreases_together(self) -> None:
        document = _config(
            application_count=16,
            dispatcher_count=16,
            budget=16,
            profile="colocated-1to1",
        )
        state = TopologyState.from_config(document)

        actions = memory_actions(
            _diagnosis("paired_reduction_required"),
            state,
            document["knobs"]["runtime"],
        )

        self.assertEqual(len(actions), 1)
        action = actions[0]
        self.assertEqual(action.name, "paired-colocated-decrease")
        self.assertEqual(action.kind, "topology")
        self.assertEqual(action.profile, "colocated-1to1")
        self.assertEqual(action.phase, SearchPhase.MEMORY)
        self.assertEqual(
            dict(action.overrides),
            {
                "knobs.runtime.application_core_count": 15,
                "knobs.runtime.dispatcher_queue_count": 15,
            },
        )
        self.assertEqual(action.impact, ImpactSpec("counter", "llc_load", "rx"))
        self.assertTrue(action.allow_equivalent_resource_reduction)

    def test_paired_reduction_uses_directional_llc_counter(self) -> None:
        actions = memory_actions(
            _diagnosis("paired_reduction_required", direction="tx"),
            self.state,
            self.runtime,
        )

        self.assertEqual(
            actions[0].impact,
            ImpactSpec("counter", "llc_store", "tx"),
        )

    def test_paired_reduction_rejects_exhausted_or_split_topology(self) -> None:
        exhausted = _config(
            application_count=1,
            dispatcher_count=1,
            budget=16,
            profile="colocated-1to1",
        )
        split = _config(
            application_count=8,
            dispatcher_count=8,
            budget=16,
            profile="split-1to1",
        )

        for document in (exhausted, split):
            with self.subTest(document=document):
                self.assertEqual(
                    memory_actions(
                        _diagnosis("paired_reduction_required"),
                        TopologyState.from_config(document),
                        document["knobs"]["runtime"],
                    ),
                    (),
                )

    def test_binary_probe_can_jump_to_any_legal_paired_count(self) -> None:
        action = paired_count_action(
            direction="rx",
            topology=self.state,
            runtime=self.runtime,
            candidate_count=4,
        )

        self.assertEqual(action.name, "paired-colocated-probe-4")
        self.assertEqual(
            dict(action.overrides),
            {
                "knobs.runtime.application_core_count": 4,
                "knobs.runtime.dispatcher_queue_count": 4,
            },
        )
        self.assertEqual(action.profile, "colocated-1to1")


class ComputeSearchPolicyTest(unittest.TestCase):
    def test_detects_compute_bottleneck_from_the_leading_completion(self) -> None:
        component = types.SimpleNamespace(
            kind="completion",
            stage="app_rx",
            name="app_rx.completion",
        )
        summary = types.SimpleNamespace(
            target=types.SimpleNamespace(leading_component=component)
        )

        self.assertEqual(
            detect_compute_bottleneck(summary),
            ComputeBottleneck.application("app_rx.completion"),
        )

    def test_application_actions_cover_split_boundary_and_complete_fanout(self) -> None:
        compact = _config(
            application_count=8,
            dispatcher_count=8,
            budget=16,
            profile="colocated-1to1",
        )
        compact_actions = compute_actions(
            ComputeBottleneck.application("app_rx.completion"),
            TopologyState.from_config(compact),
            compact["knobs"]["runtime"],
        )
        self.assertEqual(
            _names(compact_actions),
            ("split-1to1", "app-fanout-layer"),
        )
        self.assertEqual(
            compact_actions[0].impact,
            ImpactSpec("pipeline_stall", "pipeline_stall", "rx"),
        )
        self.assertEqual(
            compact_actions[1].impact,
            ImpactSpec("component", "app_rx.completion", "rx"),
        )
        self.assertEqual(
            dict(compact_actions[1].overrides),
            {"knobs.runtime.application_core_count": 16},
        )

        maximum = _config(
            application_count=16,
            dispatcher_count=16,
            budget=16,
            profile="colocated-1to1",
        )
        boundary_actions = compute_actions(
            ComputeBottleneck.application("app_tx.completion"),
            TopologyState.from_config(maximum),
            maximum["knobs"]["runtime"],
        )
        self.assertEqual(_names(boundary_actions), ("boundary-split",))
        self.assertEqual(
            boundary_actions[0].impact,
            ImpactSpec("pipeline_stall", "pipeline_stall", "tx"),
        )
        self.assertEqual(
            dict(boundary_actions[0].overrides),
            {
                "knobs.runtime.application_core_count": 8,
                "knobs.runtime.dispatcher_queue_count": 8,
            },
        )

    def test_dispatcher_actions_add_paired_growth_and_directional_c3(self) -> None:
        document = _config(
            application_count=8,
            dispatcher_count=8,
            budget=16,
            profile="colocated-1to1",
        )
        actions = compute_actions(
            ComputeBottleneck.dispatcher("dispatcher_tx.completion"),
            TopologyState.from_config(document),
            document["knobs"]["runtime"],
        )
        self.assertEqual(
            _names(actions),
            (
                "split-1to1",
                "paired-colocated-growth",
                "dispatcher-c3-tx-increase",
            ),
        )
        self.assertEqual(
            dict(actions[1].overrides),
            {
                "knobs.runtime.application_core_count": 9,
                "knobs.runtime.dispatcher_queue_count": 9,
            },
        )
        self.assertEqual(
            dict(actions[2].overrides),
            {"knobs.runtime.dispatcher_tx_batch_size": 32},
        )

    def test_cross_paired_ids_are_not_treated_as_colocated_growth(self) -> None:
        document = _config(
            application_count=8,
            dispatcher_count=8,
            budget=16,
            profile="colocated-1to1",
        )
        groups = document["deployment"]["topology"]["workloads"][0]["groups"]
        groups[0]["applications"], groups[1]["applications"] = (
            groups[1]["applications"],
            groups[0]["applications"],
        )
        state = TopologyState.from_config(document)
        self.assertEqual(state.overlap_count, 8)
        self.assertEqual(state.colocated_dispatcher_count, 6)

        actions = compute_actions(
            ComputeBottleneck.dispatcher("dispatcher_rx.completion"),
            state,
            document["knobs"]["runtime"],
        )

        self.assertEqual(
            _names(actions),
            ("split-1to1", "dispatcher-c3-rx-increase"),
        )

    def test_never_grows_dispatchers_with_application_count_fixed(self) -> None:
        for role, metric in (
            ("application", "app_rx.completion"),
            ("dispatcher", "dispatcher_rx.completion"),
        ):
            with self.subTest(role=role):
                document = _config(
                    application_count=8,
                    dispatcher_count=8,
                    budget=18,
                    profile="split-1to1",
                )
                bottleneck = ComputeBottleneck(role=role, metric=metric)
                actions = compute_actions(
                    bottleneck,
                    TopologyState.from_config(document),
                    document["knobs"]["runtime"],
                )
                if role == "application":
                    self.assertEqual(_names(actions), ("app-fanout-layer",))
                for action in actions:
                    overrides = dict(action.overrides)
                    new_a = overrides.get(
                        "knobs.runtime.application_core_count", 8
                    )
                    new_d = overrides.get(
                        "knobs.runtime.dispatcher_queue_count", 8
                    )
                    self.assertFalse(new_d > 8 and new_a == 8)


if __name__ == "__main__":
    unittest.main()
