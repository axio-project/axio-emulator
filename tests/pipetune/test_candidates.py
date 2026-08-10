from __future__ import annotations

import copy
import json
import pathlib
import tempfile
import types
import unittest
from unittest import mock

from pipetune.artifacts import write_json_atomic
from pipetune.candidates import (
    CandidateError,
    actions_for_diagnosis,
    generate_candidates,
    generate_lock_averse_candidates,
)
from pipetune.diagnosis import ProbeSpec
from pipetune.runner import MeasureError


def config_document(role: str = "server") -> dict[str, object]:
    return {
        "deployment": {
            "role": role,
            "topology": {
                "application_workspaces": [0, 1, 2, 3, 4, 5],
                "dispatcher_workspaces": [0, 1, 2, 3],
                "workspaces": [
                    {"id": index, "cpu_core": index} for index in range(6)
                ],
                "workloads": [
                    {
                        "id": 1,
                        "pipeline": ["nic_rx", "app_tx", "nic_tx"],
                        "remote_dispatchers": [0, 1],
                        "groups": [
                            {"dispatcher": 0, "applications": [0, 2]},
                            {"dispatcher": 1, "applications": [1, 3]},
                        ],
                    }
                ],
            },
        },
        "knobs": {
            "runtime": {
                "app_rx_batch_size": 16,
                "app_tx_batch_size": 16,
                "application_core_count": 4,
                "dispatcher_queue_count": 2,
                "dispatcher_rx_batch_size": 16,
                "dispatcher_tx_batch_size": 16,
                "nic_rx_post_size": 32,
                "nic_tx_post_size": 16,
            }
        },
        "network": {"backend": "dpdk"},
        "tuning": {"latency_slo_us": 100.0},
    }


def diagnosis(
    point: str,
    *,
    direction: str | None = "tx",
    required_probe: ProbeSpec | None = None,
) -> types.SimpleNamespace:
    return types.SimpleNamespace(
        point=point,
        direction=direction,
        required_probe=required_probe,
    )


class FakeCandidateTool:
    def __init__(
        self,
        *,
        leak_peer: bool = False,
        leak_peer_on_pair_call: int | None = None,
        ignore_overrides: bool = False,
    ):
        self.calls: list[tuple[str, dict[str, int]]] = []
        self.leak_peer = leak_peer
        self.leak_peer_on_pair_call = leak_peer_on_pair_call
        self.ignore_overrides = ignore_overrides
        self.pair_calls = 0

    def dump(self, path: pathlib.Path) -> dict[str, object]:
        return json.loads(path.read_text())

    @staticmethod
    def _set(document: dict[str, object], key: str, value: int) -> None:
        current: object = document
        parts = key.split(".")
        for part in parts[:-1]:
            current = current[part]
        current[parts[-1]] = value

    @staticmethod
    def _validate(document: dict[str, object]) -> None:
        runtime = document["knobs"]["runtime"]
        topology = document["deployment"]["topology"]
        c1 = runtime["application_core_count"]
        c2 = runtime["dispatcher_queue_count"]
        if not (1 <= c2 <= c1 <= len(topology["application_workspaces"])):
            raise MeasureError("invalid C1/C2")
        if c2 > len(topology["dispatcher_workspaces"]):
            raise MeasureError("invalid C2")
        for name in (
            "app_rx_batch_size",
            "app_tx_batch_size",
            "dispatcher_rx_batch_size",
            "dispatcher_tx_batch_size",
            "nic_rx_post_size",
            "nic_tx_post_size",
        ):
            value = runtime[name]
            if value < 1 or value > 64 or value & (value - 1):
                raise MeasureError("invalid C3")

    def materialize_target_pair(
        self,
        *,
        target_input: pathlib.Path,
        peer_input: pathlib.Path,
        target_output: pathlib.Path,
        peer_output: pathlib.Path,
        overrides: dict[str, int],
    ) -> None:
        self.calls.append(("pair", dict(overrides)))
        self.pair_calls += 1
        target = self.dump(target_input)
        peer = self.dump(peer_input)
        if not self.ignore_overrides:
            for key, value in overrides.items():
                self._set(target, key, value)
        self._validate(target)
        runtime = target["knobs"]["runtime"]
        workload = target["deployment"]["topology"]["workloads"][0]
        applications = target["deployment"]["topology"]["application_workspaces"]
        dispatchers = target["deployment"]["topology"]["dispatcher_workspaces"]
        c1 = runtime["application_core_count"]
        c2 = runtime["dispatcher_queue_count"]
        workload["groups"] = [
            {
                "dispatcher": dispatcher,
                "applications": list(applications[index:c1:c2]),
            }
            for index, dispatcher in enumerate(dispatchers[:c2])
        ]
        peer["deployment"]["topology"]["workloads"][0][
            "remote_dispatchers"
        ] = list(dispatchers[:c2])
        if self.leak_peer or self.pair_calls == self.leak_peer_on_pair_call:
            peer["network"]["backend"] = "roce"
        write_json_atomic(target_output, target)
        write_json_atomic(peer_output, peer)

    def materialize_target(
        self,
        *,
        target_input: pathlib.Path,
        target_output: pathlib.Path,
        overrides: dict[str, int],
    ) -> None:
        self.calls.append(("target", dict(overrides)))
        target = self.dump(target_input)
        if not self.ignore_overrides:
            for key, value in overrides.items():
                self._set(target, key, value)
        self._validate(target)
        write_json_atomic(target_output, target)

    def materialize_target_profile_pair(self, **arguments: object) -> None:
        raise AssertionError("memory candidates do not use topology profiles")

    def validate_pair(
        self, target_config: pathlib.Path, peer_config: pathlib.Path
    ) -> None:
        self._validate(self.dump(target_config))
        self._validate(self.dump(peer_config))


class CandidateActionTest(unittest.TestCase):
    def test_maps_each_diagnosis_to_ordered_single_actions(self) -> None:
        target = config_document()
        cases = (
            ("P1", "rx", ("c1-decrease", "c3-rx-increase")),
            ("P2", "tx", ("c1-decrease",)),
            ("P3", "rx", ("c2-decrease",)),
            (
                "P4",
                "tx",
                ("c1-increase", "c2-decrease", "c3-tx-decrease"),
            ),
        )
        for point, direction, expected in cases:
            with self.subTest(point=point, direction=direction):
                actions = actions_for_diagnosis(
                    diagnosis(point, direction=direction), target
                )
                self.assertEqual(tuple(action.name for action in actions), expected)
                self.assertTrue(all(len(action.overrides) in (1, 3) for action in actions))

    def test_required_probe_is_the_only_action(self) -> None:
        for direction, candidate in ((1, 5), (-1, 3)):
            with self.subTest(direction=direction):
                probe = ProbeSpec(
                    knob="knobs.runtime.application_core_count",
                    direction=direction,
                    baseline_value=4,
                    candidate_value=candidate,
                )
                actions = actions_for_diagnosis(
                    diagnosis("probe_required", required_probe=probe),
                    config_document(),
                )
                self.assertEqual(
                    tuple(action.name for action in actions), ("c1-probe",)
                )
                self.assertEqual(dict(actions[0].overrides), {probe.knob: candidate})

    def test_rejects_malformed_required_probe(self) -> None:
        cases = ((0, 4), (2, 6), (1, 3), (-1, 5))
        for direction, candidate in cases:
            with self.subTest(direction=direction, candidate=candidate):
                probe = ProbeSpec(
                    knob="knobs.runtime.application_core_count",
                    direction=direction,
                    baseline_value=4,
                    candidate_value=candidate,
                )
                with self.assertRaises(CandidateError):
                    actions_for_diagnosis(
                        diagnosis("probe_required", required_probe=probe),
                        config_document(),
                    )


class CandidateMaterializationTest(unittest.TestCase):
    def _configs(
        self, root: pathlib.Path, *, target_role: str = "server"
    ) -> tuple[pathlib.Path, pathlib.Path]:
        target = root / "target.json"
        peer = root / "peer.json"
        peer_role = "client" if target_role == "server" else "server"
        write_json_atomic(target, config_document(target_role))
        write_json_atomic(peer, config_document(peer_role))
        return target, peer

    def test_materializes_target_only_actions_for_both_target_roles(self) -> None:
        for role in ("client", "server"):
            with self.subTest(role=role), tempfile.TemporaryDirectory(
                prefix="pipetune-candidate-"
            ) as temp_dir:
                root = pathlib.Path(temp_dir)
                target, peer = self._configs(root, target_role=role)
                tool = FakeCandidateTool()
                candidates = generate_candidates(
                    diagnosis("P4", direction="tx"),
                    target_config=target,
                    peer_config=peer,
                    output_dir=root / "candidates",
                    config_tool=tool,
                )
                self.assertEqual(
                    tuple(candidate.action.name for candidate in candidates),
                    ("c1-increase", "c2-decrease", "c3-tx-decrease"),
                )
                self.assertEqual(
                    tuple(kind for kind, _ in tool.calls),
                    ("pair", "pair", "target"),
                )
                original_peer = json.loads(peer.read_text())
                for candidate in candidates:
                    materialized_peer = json.loads(candidate.peer_config.read_text())
                    if candidate.action.name == "c2-decrease":
                        self.assertNotEqual(materialized_peer, original_peer)
                        self.assertEqual(
                            materialized_peer["knobs"], original_peer["knobs"]
                        )
                    else:
                        self.assertEqual(materialized_peer, original_peer)
                self.assertTrue((root / "candidates").is_dir())
                self.assertEqual(list(root.glob(".candidates.*")), [])

    def test_materializes_legal_p4_c1_increase_and_skips_capacity_limit(self) -> None:
        for application_count, expected in (
            (4, ("c1-increase", "c3-tx-decrease")),
            (6, ("c3-tx-decrease",)),
        ):
            with self.subTest(
                application_count=application_count
            ), tempfile.TemporaryDirectory(
                prefix="pipetune-candidate-"
            ) as temp_dir:
                root = pathlib.Path(temp_dir)
                target, peer = self._configs(root)
                for path in (target, peer):
                    document = json.loads(path.read_text())
                    document["knobs"]["runtime"][
                        "application_core_count"
                    ] = application_count
                    document["knobs"]["runtime"]["dispatcher_queue_count"] = 1
                    workload = document["deployment"]["topology"]["workloads"][0]
                    workload["groups"] = [
                        {
                            "dispatcher": 0,
                            "applications": list(range(application_count)),
                        }
                    ]
                    workload["remote_dispatchers"] = [0]
                    write_json_atomic(path, document)

                candidates = generate_candidates(
                    diagnosis("P4", direction="tx"),
                    target_config=target,
                    peer_config=peer,
                    output_dir=root / "candidates",
                    config_tool=FakeCandidateTool(),
                )

                self.assertEqual(
                    tuple(candidate.action.name for candidate in candidates),
                    expected,
                )

    def test_compatibility_entry_point_uses_the_memory_policy(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-candidate-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target, peer = self._configs(root)
            document = json.loads(target.read_text())
            document["knobs"]["runtime"]["dispatcher_queue_count"] = 4
            document["deployment"]["topology"]["workloads"][0]["groups"] = [
                {"dispatcher": index, "applications": [index]}
                for index in range(4)
            ]
            write_json_atomic(target, document)
            peer_document = json.loads(peer.read_text())
            peer_document["knobs"]["runtime"]["dispatcher_queue_count"] = 4
            peer_document["deployment"]["topology"]["workloads"][0][
                "remote_dispatchers"
            ] = list(range(4))
            peer_document["deployment"]["topology"]["workloads"][0]["groups"] = [
                {"dispatcher": index, "applications": [index]}
                for index in range(4)
            ]
            write_json_atomic(peer, peer_document)

            candidates = generate_lock_averse_candidates(
                diagnosis("P4", direction="tx"),
                target_config=target,
                peer_config=peer,
                output_dir=root / "candidates",
                config_tool=FakeCandidateTool(),
            )

            self.assertEqual(
                tuple(candidate.action.name for candidate in candidates),
                ("c1-increase", "c2-decrease", "c3-tx-decrease"),
            )
            self.assertEqual(
                {path.name for path in (root / "candidates").iterdir()},
                {candidate.candidate_id for candidate in candidates},
            )
            for candidate in candidates:
                self.assertEqual(
                    candidate.canonical_target["deployment"]["role"], "server"
                )

    def test_c3_changes_the_exact_directional_triple(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-candidate-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target, peer = self._configs(root)
            candidates = generate_candidates(
                diagnosis("P1", direction="rx"),
                target_config=target,
                peer_config=peer,
                output_dir=root / "candidates",
                config_tool=FakeCandidateTool(),
            )
            rx = next(item for item in candidates if item.action.name == "c3-rx-increase")
            runtime = json.loads(rx.target_config.read_text())["knobs"]["runtime"]
            self.assertEqual(
                (
                    runtime["app_rx_batch_size"],
                    runtime["dispatcher_rx_batch_size"],
                    runtime["nic_rx_post_size"],
                ),
                (32, 32, 64),
            )
            self.assertEqual(runtime["app_tx_batch_size"], 16)

    def test_filters_invalid_candidates_and_deduplicates_canonical_pairs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-candidate-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target, peer = self._configs(root)
            document = json.loads(target.read_text())
            document["knobs"]["runtime"]["application_core_count"] = 2
            document["knobs"]["runtime"]["dispatcher_queue_count"] = 2
            document["deployment"]["topology"]["workloads"][0]["groups"] = [
                {"dispatcher": index, "applications": [index]}
                for index in range(2)
            ]
            write_json_atomic(target, document)
            peer_document = json.loads(peer.read_text())
            peer_document["knobs"]["runtime"]["application_core_count"] = 2
            peer_document["knobs"]["runtime"]["dispatcher_queue_count"] = 2
            peer_document["deployment"]["topology"]["workloads"][0][
                "groups"
            ] = [
                {"dispatcher": index, "applications": [index]}
                for index in range(2)
            ]
            peer_document["deployment"]["topology"]["workloads"][0][
                "remote_dispatchers"
            ] = [0, 1]
            write_json_atomic(peer, peer_document)
            candidates = generate_candidates(
                diagnosis("P2"),
                target_config=target,
                peer_config=peer,
                output_dir=root / "invalid",
                config_tool=FakeCandidateTool(),
            )
            self.assertEqual(candidates, ())

            with self.assertRaises(CandidateError):
                generate_candidates(
                    diagnosis("P4"),
                    target_config=target,
                    peer_config=peer,
                    output_dir=root / "no-op",
                    config_tool=FakeCandidateTool(ignore_overrides=True),
                )

            with mock.patch(
                "pipetune.candidates.canonical_config_sha256",
                return_value="same-hash",
            ):
                candidates = generate_candidates(
                    diagnosis("P4"),
                    target_config=target,
                    peer_config=peer,
                    output_dir=root / "deduplicated",
                    config_tool=FakeCandidateTool(),
                )
                self.assertEqual(candidates, ())

    def test_rejects_peer_non_route_leakage(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-candidate-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target, peer = self._configs(root)
            with self.assertRaises(CandidateError):
                generate_candidates(
                    diagnosis("P4"),
                    target_config=target,
                    peer_config=peer,
                    output_dir=root / "candidates",
                    config_tool=FakeCandidateTool(leak_peer=True),
                )

    def test_mid_generation_failure_leaves_no_partial_candidate_set(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-candidate-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target, peer = self._configs(root)
            output = root / "candidates"
            with self.assertRaises(CandidateError):
                generate_candidates(
                    diagnosis("P4"),
                    target_config=target,
                    peer_config=peer,
                    output_dir=output,
                    config_tool=FakeCandidateTool(leak_peer_on_pair_call=1),
                )
            self.assertFalse(output.exists())
            self.assertEqual(list(root.glob(".candidates.*")), [])


if __name__ == "__main__":
    unittest.main()
