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

    def test_application_actions_only_split_and_fanout_from_the_frontier(self) -> None:
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
            ImpactSpec("component", "app_rx.completion", "rx"),
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
        maximum_actions = compute_actions(
            ComputeBottleneck.application("app_tx.completion"),
            TopologyState.from_config(maximum),
            maximum["knobs"]["runtime"],
        )
        self.assertEqual(maximum_actions, ())

    def test_dispatcher_actions_only_split_the_current_frontier(self) -> None:
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
            ("split-1to1", "dispatcher-pair-growth"),
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

        self.assertEqual(actions, ())

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
                self.assertEqual(
                    _names(actions),
                    ("app-fanout-layer",) if role == "application" else ("dispatcher-pair-growth",),
                )
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
