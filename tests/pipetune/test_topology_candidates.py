from __future__ import annotations

import copy
import json
import pathlib
import tempfile
import unittest

from pipetune.artifacts import write_json_atomic
from pipetune.search_policy import (
    ComputeBottleneck,
    compute_actions,
    memory_actions,
)
from pipetune.topology_candidates import materialize_actions
from pipetune.topology_state import TopologyState
from tests.pipetune.test_search_policy import _config


class FakeProfileConfigTool:
    def __init__(self) -> None:
        self.calls: list[tuple[str, str | None]] = []

    def dump(self, path: pathlib.Path) -> dict[str, object]:
        return json.loads(path.read_text(encoding="utf-8"))

    @staticmethod
    def _set(document: dict[str, object], path: str, value: int) -> None:
        current = document
        parts = path.split(".")
        for part in parts[:-1]:
            current = current[part]
        current[parts[-1]] = value

    @staticmethod
    def _groups(
        document: dict[str, object], profile: str
    ) -> list[dict[str, object]]:
        runtime = document["knobs"]["runtime"]
        topology = document["deployment"]["topology"]
        application_count = runtime["application_core_count"]
        dispatcher_count = runtime["dispatcher_queue_count"]
        applications = topology["application_workspaces"][:application_count]
        if profile == "split-1to1":
            dispatchers = [
                value
                for value in topology["dispatcher_workspaces"]
                if value not in applications
            ][:dispatcher_count]
            return [
                {"dispatcher": dispatcher, "applications": [application]}
                for application, dispatcher in zip(applications, dispatchers)
            ]
        dispatchers = topology["dispatcher_workspaces"][:dispatcher_count]
        if profile == "colocated-1to1":
            return [
                {"dispatcher": application, "applications": [application]}
                for application in applications
            ]
        if profile == "colocated-fanout":
            return [
                {
                    "dispatcher": dispatcher,
                    "applications": applications[index::dispatcher_count],
                }
                for index, dispatcher in enumerate(dispatchers)
            ]
        raise AssertionError(profile)

    def materialize_target_profile_pair(
        self,
        *,
        target_input: pathlib.Path,
        peer_input: pathlib.Path,
        target_output: pathlib.Path,
        peer_output: pathlib.Path,
        profile: str,
        overrides: dict[str, int],
    ) -> None:
        self.calls.append(("profile", profile))
        target = self.dump(target_input)
        peer = self.dump(peer_input)
        for path, value in overrides.items():
            self._set(target, path, value)
        groups = self._groups(target, profile)
        target["deployment"]["topology"]["workloads"][0]["groups"] = groups
        peer["deployment"]["topology"]["workloads"][0][
            "remote_dispatchers"
        ] = [group["dispatcher"] for group in groups]
        write_json_atomic(target_output, target)
        write_json_atomic(peer_output, peer)

    def materialize_target_pair(self, **arguments: object) -> None:
        raise AssertionError("topology actions must use the profile materializer")

    def materialize_target(self, **arguments: object) -> None:
        self.calls.append(("target", None))
        target = self.dump(arguments["target_input"])
        for path, value in arguments["overrides"].items():
            self._set(target, path, value)
        write_json_atomic(arguments["target_output"], target)

    def validate_pair(
        self, target_config: pathlib.Path, peer_config: pathlib.Path
    ) -> None:
        TopologyState.from_config(self.dump(target_config))


class TopologyCandidateMaterializationTest(unittest.TestCase):
    def _write_pair(
        self, root: pathlib.Path, document: dict[str, object]
    ) -> tuple[pathlib.Path, pathlib.Path]:
        target = root / "target.json"
        peer = root / "peer.json"
        target_document = copy.deepcopy(document)
        peer_document = copy.deepcopy(document)
        target_document["deployment"]["role"] = "server"
        peer_document["deployment"]["role"] = "client"
        target_document["deployment"]["topology"]["workloads"][0][
            "remote_dispatchers"
        ] = []
        peer_document["deployment"]["topology"]["workloads"][0][
            "remote_dispatchers"
        ] = []
        write_json_atomic(target, target_document)
        write_json_atomic(peer, peer_document)
        return target, peer

    def test_materializes_and_validates_split_and_fanout_profiles(self) -> None:
        document = _config(
            application_count=8,
            dispatcher_count=8,
            budget=16,
            profile="colocated-1to1",
        )
        actions = compute_actions(
            ComputeBottleneck.application("app_rx.completion"),
            TopologyState.from_config(document),
            document["knobs"]["runtime"],
        )
        with tempfile.TemporaryDirectory(prefix="pipetune-topology-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target, peer = self._write_pair(root, document)
            tool = FakeProfileConfigTool()
            candidates = materialize_actions(
                actions,
                target_config=target,
                peer_config=peer,
                output_dir=root / "candidates",
                config_tool=tool,
            )

            self.assertEqual(
                tuple(candidate.action.name for candidate in candidates),
                ("split-1to1", "app-fanout-layer"),
            )
            self.assertEqual(
                tool.calls,
                [
                    ("profile", "split-1to1"),
                    ("profile", "colocated-fanout"),
                ],
            )
            states = [
                TopologyState.from_config(candidate.canonical_target)
                for candidate in candidates
            ]
            self.assertEqual(
                (states[0].overlap_count, states[0].physical_core_count),
                (0, 16),
            )
            self.assertEqual(
                (
                    states[1].application_count,
                    states[1].dispatcher_count,
                    states[1].overlap_count,
                    set(states[1].fanout_by_dispatcher.values()),
                ),
                (16, 8, 8, {2}),
            )

    def test_boundary_split_is_atomic_and_updates_only_peer_routes(self) -> None:
        document = _config(
            application_count=16,
            dispatcher_count=16,
            budget=16,
            profile="colocated-1to1",
        )
        actions = compute_actions(
            ComputeBottleneck.application("app_tx.completion"),
            TopologyState.from_config(document),
            document["knobs"]["runtime"],
        )
        with tempfile.TemporaryDirectory(prefix="pipetune-topology-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target, peer = self._write_pair(root, document)
            original_peer = json.loads(peer.read_text(encoding="utf-8"))
            candidates = materialize_actions(
                actions,
                target_config=target,
                peer_config=peer,
                output_dir=root / "candidates",
                config_tool=FakeProfileConfigTool(),
            )

            self.assertEqual(len(candidates), 1)
            state = TopologyState.from_config(candidates[0].canonical_target)
            self.assertEqual(
                (
                    state.application_count,
                    state.dispatcher_count,
                    state.overlap_count,
                    state.physical_core_count,
                ),
                (8, 8, 0, 16),
            )
            actual_peer = candidates[0].canonical_peer
            expected_peer = copy.deepcopy(original_peer)
            expected_peer["deployment"]["topology"]["workloads"][0][
                "remote_dispatchers"
            ] = actual_peer["deployment"]["topology"]["workloads"][0][
                "remote_dispatchers"
            ]
            self.assertEqual(actual_peer, expected_peer)
            self.assertTrue((root / "candidates").is_dir())
            self.assertEqual(list(root.glob(".candidates.*")), [])

    def test_materializes_repeated_paired_colocated_reductions(self) -> None:
        document = _config(
            application_count=16,
            dispatcher_count=16,
            budget=16,
            profile="colocated-1to1",
        )
        diagnosis = type(
            "Diagnosis",
            (),
            {
                "point": "paired_reduction_required",
                "direction": "rx",
                "required_probe": None,
            },
        )()
        with tempfile.TemporaryDirectory(prefix="pipetune-topology-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target, peer = self._write_pair(root, document)
            tool = FakeProfileConfigTool()
            actions = memory_actions(
                diagnosis,
                TopologyState.from_config(document),
                document["knobs"]["runtime"],
            )
            first = materialize_actions(
                actions,
                target_config=target,
                peer_config=peer,
                output_dir=root / "first",
                config_tool=tool,
            )[0]
            first_state = TopologyState.from_config(first.canonical_target)
            self.assertEqual(
                (first_state.application_count, first_state.dispatcher_count),
                (15, 15),
            )
            self.assertEqual(first_state.overlap_count, 15)
            self.assertEqual(
                first.canonical_peer["deployment"]["topology"]["workloads"][0][
                    "remote_dispatchers"
                ],
                list(range(15)),
            )

            second_actions = memory_actions(
                diagnosis,
                first_state,
                first.canonical_target["knobs"]["runtime"],
            )
            second = materialize_actions(
                second_actions,
                target_config=first.target_config,
                peer_config=first.peer_config,
                output_dir=root / "second",
                config_tool=tool,
            )[0]
            second_state = TopologyState.from_config(second.canonical_target)
            self.assertEqual(
                (second_state.application_count, second_state.dispatcher_count),
                (14, 14),
            )
            self.assertEqual(second_state.overlap_count, 14)
            self.assertEqual(tool.calls, [("profile", "colocated-1to1")] * 2)


if __name__ == "__main__":
    unittest.main()
