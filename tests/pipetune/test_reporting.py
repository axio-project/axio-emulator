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
from pipetune.paired_search import (
    PairedSearchState,
    PressureSample,
    SearchMode,
)
from pipetune.reporting import (
    ReportingError,
    _comparison_summary,
    publish_session_outputs,
    status_document,
)
from pipetune.session import ConfigPair
from tests.pipetune.test_session import (
    candidate_evaluation,
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
    def test_rejected_objective_without_metric_reports_its_reason(self) -> None:
        value = {
            "accepted": False,
            "metric": None,
            "reason": (
                "invalid: candidate enqueue drop: "
                "drop: target dispatcher enqueue"
            ),
            "observed_improvement": None,
            "required_improvement": None,
        }

        self.assertEqual(
            _comparison_summary(
                value,
                observed_field="observed_improvement",
                required_field="required_improvement",
            ),
            "reject: invalid: candidate enqueue drop: "
            "drop: target dispatcher enqueue",
        )

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
            evaluation = candidate_evaluation()
            evaluation["candidate_id"] = "p3-split-1to1"
            evaluation["trial_id"] = candidate.trial_id
            evaluation["expected_impact"]["candidate_id"] = candidate.trial_id
            evaluation["objective"]["candidate_id"] = candidate.trial_id
            evaluations = [evaluation]
            paired_search = PairedSearchState(
                direction="rx",
                mode=SearchMode.BINARY_SEEK,
                next_count=8,
                high_pressure_count=15,
                reference=PressureSample(
                    count=15,
                    llc_rate=80.0,
                    io_rate=90.0,
                    llc_uncertainty=0.5,
                    io_uncertainty=0.5,
                ),
            )
            state = store.transition(
                state,
                phase="accepted",
                accepted=best,
                details={
                    "round": 1,
                    "baseline_trial_id": baseline.trial_id,
                    "accepted_trial_id": candidate.trial_id,
                    "candidate_evaluations": evaluations,
                    "paired_search": paired_search.to_document(),
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
            self.assertIn("## Search policy", report)
            self.assertIn("NUMA workspace budget `U`", report)
            self.assertIn("memory phase", report)
            self.assertIn("compute phase", report)
            self.assertIn("never replace `best.toml`", report)
            self.assertIn("## Search cursor trajectory", report)
            self.assertIn("Binary paired search", report)
            self.assertIn("next A/D count: 8", report)
            self.assertIn("`compute`", report)
            self.assertIn("A2/D2/O2/P2 → A2/D2/O0/P4", report)
            self.assertIn("`significant_throughput`", report)
            self.assertIn("### Probes and rejected candidates", report)
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
