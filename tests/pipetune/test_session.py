from __future__ import annotations

import copy
import dataclasses
import hashlib
import json
import pathlib
import shutil
import tempfile
import unittest
from unittest import mock

from pipetune.artifacts import artifact_ref
from pipetune.model import FingerprintSet
from pipetune.paired_search import PairedSearchState
from pipetune.session import (
    ConfigPair,
    EndpointIdentity,
    SessionError,
    SessionIdentity,
    TrialAttempt,
    TuningSessionStore,
)
from tests.pipetune.test_diagnosis import build_session


def write(root: pathlib.Path, relative: str, contents: str) -> pathlib.Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents, encoding="utf-8")
    return path


def fingerprints(
    endpoint_id: str,
    binary_seed: str,
    source_sha256: str,
) -> FingerprintSet:
    return FingerprintSet(
        git_commit="b" * 40,
        binary_sha256=binary_seed * 64,
        source_config_sha256=source_sha256,
        build=f"build-{endpoint_id}",
        datapath=f"datapath-{endpoint_id}",
        deployment=f"deployment-{endpoint_id}",
    )


def create_store(root: pathlib.Path, *, details: object = None):
    source_target = write(root, "inputs/source-target.toml", "target-source\n")
    source_peer = write(root, "inputs/source-peer.toml", "peer-source\n")
    accepted_target = write(root, "configs/accepted-target.toml", "target-accepted\n")
    accepted_peer = write(root, "configs/accepted-peer.toml", "peer-accepted\n")
    source_target_ref = artifact_ref(root, source_target)
    source_peer_ref = artifact_ref(root, source_peer)
    identity = SessionIdentity(
        target=EndpointIdentity(
            endpoint_id="target",
            role="server",
            source_config=source_target_ref,
            fingerprints=fingerprints("target", "a", source_target_ref.sha256),
            canonical_config_sha256="c" * 64,
        ),
        peer=EndpointIdentity(
            endpoint_id="peer",
            role="client",
            source_config=source_peer_ref,
            fingerprints=fingerprints("peer", "d", source_peer_ref.sha256),
            canonical_config_sha256="d" * 64,
        ),
        metrics_schema="axio.metrics/v1",
        host_metrics_schema="pipetune.host-metrics/v1",
        providers=("perf", "pcm_pcie"),
    )
    accepted = ConfigPair(
        target=artifact_ref(root, accepted_target),
        peer=artifact_ref(root, accepted_peer),
    )
    store = TuningSessionStore(root)
    state = store.create(
        session_id="tuning-session-0001",
        identity=identity,
        accepted=accepted,
        details={"round": 0} if details is None else details,
    )
    return store, identity, accepted, state


def tree_snapshot(root: pathlib.Path) -> dict[str, tuple[bytes, int]]:
    return {
        path.relative_to(root).as_posix(): (path.read_bytes(), path.stat().st_mtime_ns)
        for path in root.rglob("*")
        if path.is_file()
    }


def install_complete_trial(root: pathlib.Path, trial_id: str) -> pathlib.Path:
    if trial_id != "trial-0001":
        raise ValueError("the shared diagnosis fixture uses trial-0001")
    with tempfile.TemporaryDirectory(prefix=".fixture-", dir=root) as fixture_dir:
        fixture_root = pathlib.Path(fixture_dir)
        fixture_session = build_session(fixture_root, target_role="server")
        source = fixture_session / "trials" / trial_id
        destination = root / "trials" / trial_id
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, destination)
    return destination / "trial.json"


def mutate_trial(root: pathlib.Path, mutation) -> None:
    path = root / "trials/trial-0001/trial.json"
    document = json.loads(path.read_text(encoding="utf-8"))
    mutation(document, path.parent)
    path.write_text(
        json.dumps(document, allow_nan=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def update_artifact(reference: dict[str, object], path: pathlib.Path) -> None:
    payload = path.read_bytes()
    reference["sha256"] = hashlib.sha256(payload).hexdigest()
    reference["size_bytes"] = len(payload)


def candidate_evaluation() -> dict[str, object]:
    source_topology = {
        "application_count": 2,
        "dispatcher_count": 2,
        "overlap_count": 2,
        "physical_core_count": 2,
        "physical_core_budget": 8,
        "fanout": [
            {"dispatcher": 0, "applications": 1},
            {"dispatcher": 1, "applications": 1},
        ],
    }
    candidate_topology = {
        **source_topology,
        "overlap_count": 0,
        "physical_core_count": 4,
    }
    return {
        "candidate_id": "candidate-01-split-1to1",
        "action": "split-1to1",
        "kind": "topology",
        "profile": "split-1to1",
        "search_phase": "compute",
        "impact": {
            "kind": "component",
            "metric": "app_rx.completion",
            "direction": "rx",
        },
        "source_topology": source_topology,
        "candidate_topology": candidate_topology,
        "target_sha256": "1" * 64,
        "peer_sha256": "2" * 64,
        "trial_id": "round-01-split-1to1",
        "expected_impact": {
            "candidate_id": "round-01-split-1to1",
            "point": "component",
            "metric": "app_rx.completion",
            "accepted": True,
            "reason": "expected-impact metric significantly decreases",
            "baseline_value": 0.04,
            "candidate_value": 0.03,
            "observed_reduction": 0.01,
            "required_reduction": 0.002,
            "unit": "us/packet",
        },
        "objective": {
            "candidate_id": "round-01-split-1to1",
            "accepted": True,
            "reason": "throughput improves significantly",
            "metric": "server_throughput",
            "observed_improvement": 2.0,
            "required_improvement": 0.4,
            "accepted_feasible": True,
            "candidate_feasible": True,
            "acceptance_mode": "significant_throughput",
            "physical_core_delta": 2,
        },
        "valid": True,
        "reused_probe": False,
        "reused_visited": False,
        "rejection_reason": None,
    }


class SnapshotChainTest(unittest.TestCase):
    def test_validates_topology_aware_candidate_evidence(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, _, state = create_store(root)
            evaluation = candidate_evaluation()
            state = store.checkpoint(
                state,
                details={
                    "round": 0,
                    "candidate_evaluations": [evaluation],
                    "visited_candidates": [evaluation],
                },
            )
            self.assertEqual(store.status(), state)

            corruptions = (
                ("phase", lambda value: value.__setitem__("search_phase", "third")),
                (
                    "topology",
                    lambda value: value["source_topology"].__setitem__(
                        "physical_core_count", "two"
                    ),
                ),
                ("sha", lambda value: value.__setitem__("target_sha256", "bad")),
                (
                    "acceptance",
                    lambda value: value["objective"].__setitem__(
                        "acceptance_mode", "automatic"
                    ),
                ),
            )
            for label, corrupt in corruptions:
                with self.subTest(label=label):
                    invalid = copy.deepcopy(evaluation)
                    corrupt(invalid)
                    with self.assertRaises(SessionError):
                        store.checkpoint(
                            state,
                            details={
                                "round": 0,
                                "candidate_evaluations": [invalid],
                            },
                        )
            with self.assertRaises(SessionError):
                store.checkpoint(
                    state,
                    details={
                        "round": 0,
                        "recovered_candidate_trials": [
                            {
                                "target_sha256": "bad",
                                "peer_sha256": "2" * 64,
                                "trial_id": "../trial",
                            }
                        ],
                    },
                )

    def test_history_returns_verified_generations_without_mutation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, _, baseline = create_store(root)
            store.transition(
                baseline,
                phase="diagnose",
                details={"round": 1, "diagnosis": "P3"},
            )
            before = tree_snapshot(root)

            history = store.history()

            self.assertEqual([state.generation for state in history], [0, 1])
            self.assertEqual(history[-1], store.status())
            self.assertEqual(tree_snapshot(root), before)

    def test_details_must_be_an_object_before_any_state_is_published(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            with self.assertRaisesRegex(SessionError, "details must be an object"):
                create_store(root, details=[])
            self.assertFalse((root / "session.json").exists())
            self.assertFalse(any((root / "state").glob("generation-*.json")))

    def test_retains_hash_chained_generations_and_enforces_phase_graph(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, _, baseline = create_store(root)
            generation_zero = root / "state/generation-00000000.json"
            original = generation_zero.read_bytes()

            diagnose = store.transition(
                baseline,
                phase="diagnose",
                details={"round": 1},
            )
            candidates = store.transition(
                diagnose,
                phase="candidates",
                details={"candidate_ids": ["candidate-01"]},
            )

            self.assertEqual(candidates.generation, 2)
            self.assertEqual(diagnose.previous_state_sha256, baseline.state_sha256)
            self.assertEqual(candidates.previous_state_sha256, diagnose.state_sha256)
            self.assertEqual(generation_zero.read_bytes(), original)
            self.assertEqual(len(list((root / "state").glob("generation-*.json"))), 3)
            self.assertEqual(store.status(), candidates)
            self.assertEqual(
                json.loads((root / "session.json").read_text())["schema"],
                "pipetune.tuning-session-pointer/v1",
            )
            self.assertEqual(
                json.loads(generation_zero.read_text())["schema"],
                "pipetune.tuning-session-state/v1",
            )

            before = tree_snapshot(root)
            with self.assertRaises(SessionError):
                store.transition(candidates, phase="accepted")
            self.assertEqual(tree_snapshot(root), before)

    def test_covers_the_complete_phase_graph_and_terminal_state(self) -> None:
        phases = (
            "baseline",
            "diagnose",
            "probe",
            "candidates",
            "select",
            "accepted",
            "rolled_back",
            "complete",
        )
        legal = {
            "baseline": {"diagnose", "complete"},
            "diagnose": {"probe", "candidates", "rolled_back", "complete"},
            "probe": {"diagnose", "rolled_back", "complete"},
            "candidates": {"select", "rolled_back", "complete"},
            "select": {"accepted", "rolled_back", "complete"},
            "accepted": {"diagnose", "complete"},
            "rolled_back": {"diagnose", "complete"},
            "complete": set(),
        }
        paths = {
            "baseline": (),
            "diagnose": ("diagnose",),
            "probe": ("diagnose", "probe"),
            "candidates": ("diagnose", "candidates"),
            "select": ("diagnose", "candidates", "select"),
            "accepted": ("diagnose", "candidates", "select", "accepted"),
            "rolled_back": (
                "diagnose",
                "candidates",
                "select",
                "rolled_back",
            ),
            "complete": ("complete",),
        }
        for source in phases:
            for destination in phases:
                with self.subTest(
                    source=source, destination=destination
                ), tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
                    store, _, _, state = create_store(pathlib.Path(temp_dir))
                    for phase in paths[source]:
                        state = store.transition(state, phase=phase)
                    if destination in legal[source]:
                        result = store.transition(state, phase=destination)
                        self.assertEqual(result.phase, destination)
                    else:
                        before = tree_snapshot(pathlib.Path(temp_dir))
                        with self.assertRaises(SessionError):
                            store.transition(state, phase=destination)
                        self.assertEqual(tree_snapshot(pathlib.Path(temp_dir)), before)

    def test_stale_transition_is_compare_and_swap_safe(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, _, baseline = create_store(root)
            store.transition(baseline, phase="diagnose", details={"round": 1})
            before = tree_snapshot(root)
            with self.assertRaisesRegex(SessionError, "stale"):
                store.transition(baseline, phase="complete")
            self.assertEqual(tree_snapshot(root), before)

    def test_mutated_in_memory_state_fails_hash_check_before_transition(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, _, baseline = create_store(root)
            baseline.details["round"] = 99
            before = tree_snapshot(root)
            with self.assertRaisesRegex(SessionError, "state object hash"):
                store.transition(baseline, phase="diagnose")
            self.assertEqual(tree_snapshot(root), before)

    def test_detects_old_generation_or_pointer_tampering(self) -> None:
        for target in ("state", "pointer"):
            with self.subTest(target=target), tempfile.TemporaryDirectory(
                prefix="pipetune-session-"
            ) as temp_dir:
                root = pathlib.Path(temp_dir)
                store, _, _, baseline = create_store(root)
                store.transition(baseline, phase="diagnose")
                path = (
                    root / "state/generation-00000000.json"
                    if target == "state"
                    else root / "session.json"
                )
                document = json.loads(path.read_text(encoding="utf-8"))
                if target == "state":
                    document["details"] = {"tampered": True}
                else:
                    document["state_sha256"] = "0" * 64
                path.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaises(SessionError):
                    store.status()

    def test_rejects_a_rehashed_illegal_persisted_transition(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, _, baseline = create_store(root)
            store.transition(baseline, phase="diagnose")
            state_path = root / "state/generation-00000001.json"
            state = json.loads(state_path.read_text(encoding="utf-8"))
            state["phase"] = "accepted"
            payload = (
                json.dumps(state, allow_nan=False, indent=2, sort_keys=True) + "\n"
            ).encode("utf-8")
            state_path.write_bytes(payload)
            pointer_path = root / "session.json"
            pointer = json.loads(pointer_path.read_text(encoding="utf-8"))
            pointer["state_sha256"] = hashlib.sha256(payload).hexdigest()
            pointer_path.write_text(
                json.dumps(pointer, allow_nan=False, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(SessionError, "persisted phase"):
                store.status()

    def test_status_is_strictly_read_only(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, _, state = create_store(root)
            before = tree_snapshot(root)
            self.assertEqual(store.status(), state)
            self.assertEqual(tree_snapshot(root), before)


class ResumeValidationTest(unittest.TestCase):
    def test_session_rejects_invalid_paired_search_evidence(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            document = PairedSearchState.start(
                direction="rx", count=16
            ).to_document()
            document["low_relief_count"] = 9
            document["high_pressure_count"] = 2

            with self.assertRaisesRegex(SessionError, "paired_search"):
                create_store(
                    pathlib.Path(temp_dir),
                    details={"round": 0, "paired_search": document},
                )

    def test_resume_rejects_identity_drift(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, identity, _, _ = create_store(root)
            changed_target = dataclasses.replace(
                identity.target,
                fingerprints=dataclasses.replace(
                    identity.target.fingerprints,
                    binary_sha256="e" * 64,
                ),
            )
            with self.assertRaisesRegex(SessionError, "identity"):
                store.resume(dataclasses.replace(identity, target=changed_target))

    def test_endpoint_identity_requires_its_source_config_hash(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            _, identity, _, _ = create_store(root)
            with self.assertRaisesRegex(SessionError, "source config"):
                dataclasses.replace(
                    identity.target,
                    fingerprints=dataclasses.replace(
                        identity.target.fingerprints,
                        source_config_sha256="f" * 64,
                    ),
                )

    def test_resume_rejects_source_and_accepted_config_mutation(self) -> None:
        for relative in ("inputs/source-target.toml", "configs/accepted-peer.toml"):
            with self.subTest(relative=relative), tempfile.TemporaryDirectory(
                prefix="pipetune-session-"
            ) as temp_dir:
                root = pathlib.Path(temp_dir)
                store, identity, _, _ = create_store(root)
                write(root, relative, "mutated\n")
                with self.assertRaises(SessionError):
                    store.resume(identity)

    def test_reuses_complete_orphan_after_atomic_pointer_failure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, _, baseline = create_store(root)
            from pipetune import session as session_module

            real_write = session_module.write_json_atomic

            def fail_pointer(path: pathlib.Path, document: object) -> None:
                if path.name == "session.json":
                    raise OSError("simulated pointer crash")
                real_write(path, document)

            with mock.patch(
                "pipetune.session.write_json_atomic", side_effect=fail_pointer
            ), self.assertRaises(OSError):
                store.transition(baseline, phase="diagnose", details={"round": 1})

            self.assertEqual(store.status().generation, 0)
            orphan = root / "state/generation-00000001.json"
            orphan_bytes = orphan.read_bytes()
            recovered = store.transition(
                baseline,
                phase="diagnose",
                details={"round": 1},
            )
            self.assertEqual(recovered.generation, 1)
            self.assertEqual(orphan.read_bytes(), orphan_bytes)

    def test_refuses_a_divergent_existing_orphan_without_overwrite(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, _, baseline = create_store(root)
            orphan = root / "state/generation-00000001.json"
            orphan.parent.mkdir(parents=True, exist_ok=True)
            orphan.write_text('{"divergent":true}\n', encoding="utf-8")
            before = tree_snapshot(root)
            with self.assertRaisesRegex(SessionError, "orphan"):
                store.transition(baseline, phase="diagnose")
            self.assertEqual(tree_snapshot(root), before)

    def test_cleanup_of_owned_temporary_files_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, _, _ = create_store(root)
            write(root, ".session.json.crash.tmp", "temporary")
            write(root, "state/.generation-00000001.json.crash.tmp", "temporary")
            keep = write(root, "state/keep.tmp", "unrelated")
            self.assertEqual(store.cleanup_temporary_files(), 2)
            self.assertEqual(store.cleanup_temporary_files(), 0)
            self.assertEqual(keep.read_text(encoding="utf-8"), "unrelated")


class InterruptedTrialTest(unittest.TestCase):
    def test_finalizes_complete_trial_and_verifies_provider_hashes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, identity, _, baseline = create_store(root)
            diagnose = store.transition(baseline, phase="diagnose")
            running = store.start_trial(diagnose, "trial-0001")
            install_complete_trial(root, "trial-0001")

            recovery = store.resume(identity)
            self.assertEqual(recovery.action, "finalized")
            self.assertEqual(recovery.trial_id, "trial-0001")
            self.assertEqual(recovery.state.attempts[-1].status, "complete")
            repeated = store.resume(identity)
            self.assertEqual(repeated.action, "none")
            self.assertEqual(repeated.state.state_sha256, recovery.state.state_sha256)

            raw_provider = next(
                path
                for path in (root / "trials/trial-0001").rglob("raw.txt")
            )
            raw_provider.write_text("tampered\n", encoding="utf-8")
            with self.assertRaises(SessionError):
                store.status()

    def test_refuses_session_incompatible_trial_evidence(self) -> None:
        cases = (
            "roles",
            "git",
            "binary",
            "source_hash",
            "canonical",
            "metrics",
            "providers",
        )
        for case in cases:
            with self.subTest(case=case), tempfile.TemporaryDirectory(
                prefix="pipetune-session-"
            ) as temp_dir:
                root = pathlib.Path(temp_dir)
                store, identity, _, baseline = create_store(root)
                diagnose = store.transition(baseline, phase="diagnose")
                store.start_trial(diagnose, "trial-0001")
                install_complete_trial(root, "trial-0001")

                def mutation(document, trial_root):
                    endpoints = {
                        item["spec"]["endpoint_id"]: item
                        for item in document["endpoints"]
                    }
                    target = endpoints["target"]
                    if case == "roles":
                        target["spec"]["role"] = "client"
                        endpoints["peer"]["spec"]["role"] = "server"
                    elif case == "git":
                        target["fingerprints"]["git_commit"] = "c" * 40
                    elif case == "binary":
                        target["fingerprints"]["binary_sha256"] = "e" * 64
                    elif case == "source_hash":
                        target["fingerprints"]["source_config_sha256"] = "f" * 64
                    elif case == "canonical":
                        reference = next(
                            item
                            for item in target["artifacts"]
                            if item["path"] == "configs/canonical/target.json"
                        )
                        path = trial_root / reference["path"]
                        path.write_text("[]\n", encoding="utf-8")
                        update_artifact(reference, path)
                    elif case == "metrics":
                        reference = next(
                            item
                            for item in target["artifacts"]
                            if item["schema"] == "axio.metrics/v1"
                        )
                        path = trial_root / reference["path"]
                        path.write_text('{"malformed":true}\n', encoding="utf-8")
                        update_artifact(reference, path)
                    else:
                        reference = document["host_metrics"]
                        path = trial_root / reference["path"]
                        host_metrics = json.loads(path.read_text(encoding="utf-8"))
                        host_metrics["providers"].reverse()
                        path.write_text(
                            json.dumps(
                                host_metrics,
                                allow_nan=False,
                                indent=2,
                                sort_keys=True,
                            )
                            + "\n",
                            encoding="utf-8",
                        )
                        update_artifact(reference, path)

                mutate_trial(root, mutation)
                with self.assertRaises(SessionError):
                    store.resume(identity)

    def test_allows_only_one_pending_or_running_attempt(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, _, baseline = create_store(root)
            diagnose = store.transition(baseline, phase="diagnose")
            running = store.start_trial(diagnose, "trial-0001")
            pending = TrialAttempt(
                trial_id="trial-0002",
                status="pending",
                manifest_path="trials/trial-0002/trial.json",
                manifest=None,
            )
            before = tree_snapshot(root)
            with self.assertRaisesRegex(SessionError, "active trial"):
                store.transition(
                    running,
                    phase="candidates",
                    attempts=running.attempts + (pending,),
                )
            self.assertEqual(tree_snapshot(root), before)

    def test_start_trial_refuses_a_preexisting_trial_directory(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, _, baseline = create_store(root)
            diagnose = store.transition(baseline, phase="diagnose")
            write(root, "trials/trial-existing/unrelated", "do not adopt\n")
            before = tree_snapshot(root)
            with self.assertRaisesRegex(SessionError, "already exists"):
                store.start_trial(diagnose, "trial-existing")
            self.assertEqual(tree_snapshot(root), before)

    def test_missing_manifest_reruns_under_new_id_once(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, identity, _, baseline = create_store(root)
            diagnose = store.transition(baseline, phase="diagnose")
            running = store.start_trial(diagnose, "trial-0001")

            recovery = store.resume(identity, retry_trial_id="trial-0002")
            self.assertEqual(recovery.action, "rerun")
            self.assertEqual(recovery.trial_id, "trial-0002")
            self.assertEqual(
                tuple(attempt.status for attempt in recovery.state.attempts),
                ("abandoned", "pending"),
            )
            repeated = store.resume(identity, retry_trial_id="trial-0002")
            self.assertEqual(repeated.action, "none")
            self.assertEqual(repeated.state.state_sha256, recovery.state.state_sha256)
            self.assertGreater(recovery.state.generation, running.generation)
            before = tree_snapshot(root)
            with self.assertRaisesRegex(SessionError, "pending retry"):
                store.resume(identity, retry_trial_id="trial-0003")
            self.assertEqual(tree_snapshot(root), before)

    def test_retry_refuses_a_preexisting_new_trial_directory(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, identity, _, baseline = create_store(root)
            diagnose = store.transition(baseline, phase="diagnose")
            running = store.start_trial(diagnose, "trial-0001")
            write(root, "trials/trial-0002/unrelated", "do not adopt\n")
            before = tree_snapshot(root)
            with self.assertRaisesRegex(SessionError, "already exists"):
                store.resume(identity, retry_trial_id="trial-0002")
            self.assertEqual(tree_snapshot(root), before)
            self.assertEqual(store.status().state_sha256, running.state_sha256)

    def test_partially_published_manifest_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, identity, _, baseline = create_store(root)
            diagnose = store.transition(baseline, phase="diagnose")
            running = store.start_trial(diagnose, "trial-0001")
            write(root, "trials/trial-0001/trial.json", "{\"partial\": true}\n")

            with self.assertRaises(SessionError):
                store.resume(identity)
            self.assertEqual(store.status().state_sha256, running.state_sha256)

    def test_completed_configs_remain_immutable_across_acceptance(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, accepted, baseline = create_store(root)
            diagnose = store.transition(baseline, phase="diagnose")
            candidates = store.transition(diagnose, phase="candidates")
            select = store.transition(candidates, phase="select")
            new_target = write(root, "configs/accepted-target-2.toml", "new-target\n")
            new_peer = write(root, "configs/accepted-peer-2.toml", "new-peer\n")
            new_pair = ConfigPair(
                target=artifact_ref(root, new_target),
                peer=artifact_ref(root, new_peer),
            )
            store.transition(select, phase="accepted", accepted=new_pair)

            write(root, accepted.target.path, "overwritten\n")
            with self.assertRaises(SessionError):
                store.status()


if __name__ == "__main__":
    unittest.main()
