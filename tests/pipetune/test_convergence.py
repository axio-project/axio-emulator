from __future__ import annotations

import dataclasses
import pathlib
import tempfile
import unittest

from pipetune.artifacts import artifact_ref
from pipetune.controller import (
    ConvergenceResult,
    InfrastructureFailureLimit,
    RoundResult,
    TuningLoop,
    _round_boundary_document,
)
from pipetune.diagnosis import Diagnosis, Statistic
from pipetune.objective import ObjectivePolicy, ObjectiveTrial
from pipetune.session import ConfigPair, SessionState, TuningSessionStore
from tests.pipetune.test_session import create_store


POLICY = ObjectivePolicy(
    latency_slo_us=100.0,
    latency_relative_floor=0.03,
    throughput_relative_floor=0.01,
)


def _statistic(value: float, unit: str) -> Statistic:
    return Statistic(
        samples=(value,),
        median=value,
        mad=0.0,
        uncertainty=value * 0.01,
        unit=unit,
    )


def _objective(
    trial_id: str,
    throughput: float,
    *,
    latency: float = 2.0,
) -> ObjectiveTrial:
    return ObjectiveTrial(
        trial_id=trial_id,
        status="valid",
        client_p999=_statistic(latency, "us"),
        server_throughput=_statistic(throughput, "Mpps"),
        rejection_reason=None,
    )


def _diagnosis() -> Diagnosis:
    return Diagnosis(
        schema="pipetune.diagnosis/v1",
        point="P3",
        direction="rx",
        confidence="high",
        evidence=(),
        rejected_evidence=(),
        missing_metrics=(),
        noise_thresholds={},
        required_probe=None,
        input_hashes={},
        stage_ranking=(),
        completed_probe=None,
    )


@dataclasses.dataclass(frozen=True)
class RoundScript:
    baseline_mpps: float
    candidate_mpps: float | None
    rollback_reason: str | None = None
    baseline_latency_us: float = 2.0
    candidate_latency_us: float = 2.0


class ScriptedRoundRunner:
    def __init__(
        self,
        root: pathlib.Path,
        store: TuningSessionStore,
        scripts: tuple[RoundScript, ...],
    ) -> None:
        self.root = root
        self.store = store
        self.scripts = scripts
        self.calls: list[int] = []

    def run_round(
        self,
        state: SessionState,
        *,
        round_index: int,
        round_attempt: int = 1,
    ) -> RoundResult:
        del round_attempt
        self.calls.append(round_index)
        script = self.scripts[len(self.calls) - 1]
        previous = state.accepted
        baseline = _objective(
            f"round-{round_index:02d}-baseline",
            script.baseline_mpps,
            latency=script.baseline_latency_us,
        )
        state = self.store.transition(state, phase="diagnose")
        state = self.store.transition(state, phase="candidates")
        state = self.store.transition(state, phase="select")
        accepted_objective = None
        accepted_trial_id = None
        if script.candidate_mpps is None:
            state = self.store.transition(
                state,
                phase="rolled_back",
                details={
                    "round": round_index,
                    "reason": script.rollback_reason,
                    "round_boundary": _round_boundary_document(
                        round_index=round_index,
                        previous_accepted=previous,
                        baseline_objective=baseline,
                        accepted_objective=None,
                        accepted_trial_id=None,
                        rollback_reason=script.rollback_reason,
                    ),
                },
            )
        else:
            target = self.root / f"configs/round-{round_index:02d}-target.toml"
            peer = self.root / f"configs/round-{round_index:02d}-peer.toml"
            target.write_text(f"target round {round_index}\n", encoding="utf-8")
            peer.write_text(f"peer round {round_index}\n", encoding="utf-8")
            pair = ConfigPair(
                target=artifact_ref(self.root, target),
                peer=artifact_ref(self.root, peer),
            )
            accepted_trial_id = f"round-{round_index:02d}-candidate"
            accepted_objective = _objective(
                accepted_trial_id,
                script.candidate_mpps,
                latency=script.candidate_latency_us,
            )
            state = self.store.transition(
                state,
                phase="accepted",
                accepted=pair,
                details={
                    "round": round_index,
                    "round_boundary": _round_boundary_document(
                        round_index=round_index,
                        previous_accepted=previous,
                        baseline_objective=baseline,
                        accepted_objective=accepted_objective,
                        accepted_trial_id=accepted_trial_id,
                        rollback_reason=None,
                    ),
                },
            )
        return RoundResult(
            state=state,
            previous_accepted=previous,
            diagnosis=_diagnosis(),
            baseline_trial_id=baseline.trial_id,
            probe_trial_id=None,
            candidate_trials=(accepted_trial_id,) if accepted_trial_id else (),
            accepted_trial_id=accepted_trial_id,
            reused_probe=False,
            comparisons=(),
            expected_impacts=(),
            baseline_objective=baseline,
            accepted_objective=accepted_objective,
        )


class TuningLoopTest(unittest.TestCase):
    def _run(
        self,
        root: pathlib.Path,
        scripts: tuple[RoundScript, ...],
        *,
        max_iterations: int,
    ) -> tuple[ConvergenceResult, ScriptedRoundRunner]:
        store, _identity, _accepted, state = create_store(root)
        runner = ScriptedRoundRunner(root, store, scripts)
        result = TuningLoop(
            store=store,
            round_runner=runner,
            policy=POLICY,
        ).run(state, max_iterations=max_iterations)
        return result, runner

    def test_stops_on_no_legal_candidate(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-loop-") as temp_dir:
            result, runner = self._run(
                pathlib.Path(temp_dir),
                (RoundScript(40.0, None, "no legal candidate"),),
                max_iterations=4,
            )
            self.assertEqual(result.stop_reason, "no_legal_candidate")
            self.assertEqual(result.completed_rounds, 1)
            self.assertEqual(runner.calls, [1])
            self.assertEqual(result.state.phase, "complete")
            self.assertEqual(result.best, result.rounds[0].previous_accepted)
            self.assertEqual(
                result.best_trial_id,
                result.rounds[0].baseline_trial_id,
            )

    def test_stops_on_no_significant_improvement(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-loop-") as temp_dir:
            result, runner = self._run(
                pathlib.Path(temp_dir),
                (RoundScript(40.0, None, "no significant improvement"),),
                max_iterations=4,
            )
            self.assertEqual(result.stop_reason, "no_significant_improvement")
            self.assertEqual(result.completed_rounds, 1)
            self.assertEqual(runner.calls, [1])

    def test_stops_when_all_candidates_are_invalid(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-loop-") as temp_dir:
            result, runner = self._run(
                pathlib.Path(temp_dir),
                (RoundScript(40.0, None, "all candidates invalid"),),
                max_iterations=4,
            )
            self.assertEqual(result.stop_reason, "all_candidates_invalid")
            self.assertEqual(result.completed_rounds, 1)
            self.assertEqual(runner.calls, [1])

    def test_latency_feasibility_does_not_stop_before_iteration_limit(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-loop-") as temp_dir:
            result, runner = self._run(
                pathlib.Path(temp_dir),
                (
                    RoundScript(
                        40.0,
                        42.0,
                        baseline_latency_us=150.0,
                        candidate_latency_us=120.0,
                    ),
                    RoundScript(
                        42.0,
                        43.0,
                        baseline_latency_us=120.0,
                        candidate_latency_us=90.0,
                    ),
                ),
                max_iterations=2,
            )
            self.assertEqual(result.stop_reason, "max_iterations")
            self.assertEqual(result.completed_rounds, 2)
            self.assertEqual(runner.calls, [1, 2])
            self.assertLessEqual(
                result.rounds[-1].accepted_objective.client_p999.median,
                POLICY.latency_slo_us,
            )

    def test_returns_historical_best_instead_of_final_accepted_pair(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-loop-") as temp_dir:
            result, _runner = self._run(
                pathlib.Path(temp_dir),
                (
                    RoundScript(40.0, 45.0),
                    RoundScript(41.0, 42.0),
                ),
                max_iterations=2,
            )
            self.assertEqual(result.best_trial_id, "round-01-candidate")
            self.assertEqual(
                result.best.target.path,
                "configs/round-01-target.toml",
            )
            self.assertNotEqual(result.best, result.state.accepted)
            restored_store = TuningSessionStore(pathlib.Path(temp_dir))
            restored_state = restored_store.status()
            restored_runner = ScriptedRoundRunner(
                pathlib.Path(temp_dir),
                restored_store,
                (),
            )
            restored_loop = TuningLoop(
                store=restored_store,
                round_runner=restored_runner,
                policy=POLICY,
            )
            snapshot = restored_loop.inspect(restored_state)
            self.assertEqual(snapshot.completed_rounds, 2)
            self.assertEqual(snapshot.stop_reason, "max_iterations")
            self.assertEqual(snapshot.best, result.best)
            self.assertEqual(snapshot.best_trial_id, result.best_trial_id)
            resumed = restored_loop.run(restored_state, max_iterations=2)
            self.assertEqual(resumed.best, result.best)
            self.assertEqual(resumed.completed_rounds, 2)
            self.assertEqual(restored_runner.calls, [])

    def test_completes_at_bounded_infrastructure_failure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-loop-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, accepted, state = create_store(root)

            class FailedRunner:
                def run_round(self, current, *, round_index, round_attempt=1):
                    del round_attempt
                    self.calls += 1
                    raise InfrastructureFailureLimit(
                        state=current,
                        consecutive_failures=2,
                        reason="scripted endpoint failure",
                    )

                calls = 0

            runner = FailedRunner()
            result = TuningLoop(
                store=store,
                round_runner=runner,
                policy=POLICY,
            ).run(state, max_iterations=4)
            self.assertEqual(result.stop_reason, "infrastructure_failure_limit")
            self.assertEqual(result.completed_rounds, 0)
            self.assertEqual(result.infrastructure_failures, 2)
            self.assertEqual(result.best, accepted)
            self.assertIsNone(result.best_trial_id)
            self.assertEqual(result.state.phase, "complete")
            self.assertEqual(runner.calls, 1)

    def test_rejects_invalid_iteration_limit(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-loop-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            runner = ScriptedRoundRunner(root, store, ())
            loop = TuningLoop(store=store, round_runner=runner, policy=POLICY)
            for value in (0, -1, True):
                with self.subTest(value=value):
                    with self.assertRaisesRegex(ValueError, "max iterations"):
                        loop.run(state, max_iterations=value)

    def test_recovers_an_accepted_boundary_before_its_checkpoint(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-loop-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, _accepted, state = create_store(root)
            first_runner = ScriptedRoundRunner(
                root,
                store,
                (RoundScript(40.0, 42.0),),
            )
            first = first_runner.run_round(state, round_index=1)
            restored_store = TuningSessionStore(root)
            second_runner = ScriptedRoundRunner(
                root,
                restored_store,
                (RoundScript(42.0, 43.0),),
            )
            resumed = TuningLoop(
                store=restored_store,
                round_runner=second_runner,
                policy=POLICY,
            ).run(restored_store.status(), max_iterations=2)
            self.assertEqual(second_runner.calls, [2])
            self.assertEqual(resumed.completed_rounds, 2)

    def test_recovers_a_rolled_back_boundary_without_another_round(self) -> None:
        for reason, expected in (
            ("no significant improvement", "no_significant_improvement"),
            ("all candidates invalid", "all_candidates_invalid"),
        ):
            with self.subTest(reason=reason), tempfile.TemporaryDirectory(
                prefix="pipetune-loop-"
            ) as temp_dir:
                root = pathlib.Path(temp_dir)
                store, _identity, _accepted, state = create_store(root)
                first_runner = ScriptedRoundRunner(
                    root,
                    store,
                    (RoundScript(40.0, None, reason),),
                )
                first_runner.run_round(state, round_index=1)
                restored_store = TuningSessionStore(root)
                should_not_run = ScriptedRoundRunner(root, restored_store, ())
                resumed = TuningLoop(
                    store=restored_store,
                    round_runner=should_not_run,
                    policy=POLICY,
                ).run(restored_store.status(), max_iterations=4)
                self.assertEqual(should_not_run.calls, [])
                self.assertEqual(resumed.stop_reason, expected)
                self.assertEqual(resumed.completed_rounds, 1)


if __name__ == "__main__":
    unittest.main()
