from __future__ import annotations

import dataclasses
import json
import pathlib
import tempfile
import unittest

from pipetune.artifacts import artifact_ref
from pipetune.controller import ConvergenceResult, _round_boundary_document
from pipetune.diagnosis import Statistic
from pipetune.objective import ObjectiveTrial
from pipetune.reporting import (
    ReportingError,
    publish_session_outputs,
    status_document,
)
from pipetune.session import ConfigPair
from tests.pipetune.test_session import (
    create_store,
    install_complete_trial,
    tree_snapshot,
    write,
)


def _statistic(value: float, unit: str) -> Statistic:
    return Statistic((value,), value, 0.0, value * 0.01, unit)


def _objective(trial_id: str, throughput: float) -> ObjectiveTrial:
    return ObjectiveTrial(
        trial_id=trial_id,
        status="valid",
        client_p999=_statistic(2.0, "us"),
        server_throughput=_statistic(throughput, "Mpps"),
        rejection_reason=None,
    )


def _diagnosis_document() -> dict[str, object]:
    statistic = {
        "mad": 0.1,
        "median": 5.0,
        "sample_count": 3,
        "uncertainty": 0.5,
        "unit": "percent",
    }
    return {
        "schema": "pipetune.diagnosis/v1",
        "counter_rates": {
            "baseline": {
                "llc_load": statistic,
                "llc_store": statistic,
                "io_read": statistic,
                "io_write": statistic,
            },
            "probe": None,
        },
        "result": {
            "point": "P3",
            "direction": "rx",
            "confidence": "high",
            "evidence": [],
            "rejected_evidence": [],
            "missing_metrics": [],
        },
        "trial": {"baseline": "round-01-baseline", "probe": None},
    }


class ReportingTest(unittest.TestCase):
    def test_publishes_historical_best_pair_and_auditable_iteration(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-report-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, initial, state = create_store(
                root,
                details={
                    "round": 0,
                    "bootstrap": {
                        "schema": "pipetune.bootstrap/v1",
                        "max_iterations": 1,
                    },
                },
            )
            baseline = _objective("round-01-baseline", 40.0)
            candidate = _objective("round-01-candidate", 42.0)
            state = store.transition(
                state,
                phase="diagnose",
                details={
                    "round": 1,
                    "diagnosis": "P3",
                    "diagnosis_document": _diagnosis_document(),
                },
            )
            state = store.transition(state, phase="candidates")
            state = store.transition(state, phase="select")
            best_target = write(root, "configs/best-target.toml", "best target\n")
            best_peer = write(root, "configs/best-peer.toml", "best peer\n")
            best = ConfigPair(
                target=artifact_ref(root, best_target),
                peer=artifact_ref(root, best_peer),
            )
            evaluations = [
                {
                    "candidate_id": "p3-c2-decrease",
                    "action": "c2-decrease",
                    "trial_id": "round-01-candidate",
                    "expected_impact": {
                        "accepted": True,
                        "metric": "io_write",
                        "reason": "expected impact decreased significantly",
                    },
                    "objective": {
                        "accepted": True,
                        "metric": "server_throughput",
                        "reason": "throughput improved significantly",
                    },
                    "valid": True,
                }
            ]
            state = store.transition(
                state,
                phase="accepted",
                accepted=best,
                details={
                    "round": 1,
                    "baseline_trial_id": baseline.trial_id,
                    "accepted_trial_id": candidate.trial_id,
                    "candidate_evaluations": evaluations,
                    "diagnosis_document": _diagnosis_document(),
                    "round_boundary": _round_boundary_document(
                        round_index=1,
                        previous_accepted=initial,
                        baseline_objective=baseline,
                        accepted_objective=candidate,
                        accepted_trial_id=candidate.trial_id,
                        rollback_reason=None,
                    ),
                },
            )
            state = store.transition(
                state,
                phase="complete",
                details={
                    **state.details,
                    "convergence": {
                        "completed_rounds": 1,
                        "best": {
                            "target": dataclasses.asdict(best.target),
                            "peer": dataclasses.asdict(best.peer),
                        },
                        "best_trial_id": candidate.trial_id,
                        "best_objective": _round_boundary_document(
                            round_index=1,
                            previous_accepted=initial,
                            baseline_objective=baseline,
                            accepted_objective=candidate,
                            accepted_trial_id=candidate.trial_id,
                            rollback_reason=None,
                        )["accepted_objective"],
                        "stop_reason": "max_iterations",
                        "infrastructure_failures": 0,
                    },
                },
            )
            result = ConvergenceResult(
                state=state,
                rounds=(),
                stop_reason="max_iterations",
                completed_rounds=1,
                infrastructure_failures=0,
                best=best,
                best_trial_id=candidate.trial_id,
            )

            publication = publish_session_outputs(root, store, result)

            self.assertEqual((root / "best.toml").read_text(), "best target\n")
            self.assertEqual((root / "peer.toml").read_text(), "best peer\n")
            records = [
                json.loads(line)
                for line in (root / "iterations.jsonl").read_text().splitlines()
            ]
            self.assertEqual(len(records), 1)
            self.assertEqual(
                set(records[0]["diagnosis"]["counter_rates"]["baseline"]),
                {"llc_load", "llc_store", "io_read", "io_write"},
            )
            self.assertTrue(
                records[0]["candidates"][0]["expected_impact"]["accepted"]
            )
            self.assertTrue(records[0]["candidates"][0]["objective"]["accepted"])
            report = (root / "report.md").read_text()
            self.assertIn("Expected impact", report)
            self.assertIn("End-to-end objective", report)
            self.assertIn("C4", report)
            self.assertNotIn(str(root), report)
            self.assertEqual(publication.stop_reason, "max_iterations")
            with self.assertRaisesRegex(
                ReportingError, "does not match persisted state"
            ):
                publish_session_outputs(
                    root,
                    store,
                    dataclasses.replace(result, stop_reason="all_candidates_invalid"),
                )

    def test_terminal_invalid_control_links_the_published_attempt(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-report-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, identity, accepted, state = create_store(root)
            store.start_trial(state, "trial-0001")
            install_complete_trial(root, "trial-0001")
            finalized = store.resume(identity)
            self.assertEqual(finalized.action, "finalized")
            state = store.transition(
                finalized.state,
                phase="complete",
                details={
                    "round": 1,
                    "invalid_control_evidence": {
                        "label": "baseline",
                        "reason": "baseline objective is structurally invalid",
                        "trial_id": "trial-0001",
                    },
                    "convergence": {
                        "best": {
                            "target": dataclasses.asdict(accepted.target),
                            "peer": dataclasses.asdict(accepted.peer),
                        },
                        "best_objective": None,
                        "best_trial_id": None,
                        "completed_rounds": 0,
                        "infrastructure_failures": 0,
                        "stop_reason": "invalid_control_evidence",
                    },
                },
            )
            result = ConvergenceResult(
                state=state,
                rounds=(),
                stop_reason="invalid_control_evidence",
                completed_rounds=0,
                infrastructure_failures=0,
                best=accepted,
                best_trial_id=None,
            )

            publish_session_outputs(root, store, result)

            record = json.loads((root / "iterations.jsonl").read_text())
            self.assertEqual(record["outcome"]["kind"], "terminated")
            self.assertEqual(record["baseline"]["trial_id"], "trial-0001")
            self.assertEqual(record["attempts"][0]["status"], "complete")
            report = (root / "report.md").read_text()
            self.assertIn("trials/trial-0001/trial.json", report)
            self.assertIn("invalid_control_evidence", report)

    def test_read_only_status_does_not_republish_outputs(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-status-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _, _, _ = create_store(root)
            before = tree_snapshot(root)

            document = status_document(root, store)

            self.assertEqual(document["phase"], "baseline")
            self.assertEqual(tree_snapshot(root), before)


if __name__ == "__main__":
    unittest.main()
