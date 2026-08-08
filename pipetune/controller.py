"""Cold-start diagnosis and candidate orchestration for PipeTune."""

from __future__ import annotations

import dataclasses
import os
import pathlib
import shutil
import tempfile
import uuid
from typing import Callable, Protocol

from pipetune.artifacts import artifact_ref
from pipetune.candidates import Candidate, CandidateConfigTool, generate_candidates
from pipetune.diagnosis import Diagnosis, SteadySummary, diagnose_summary, summarize_trial
from pipetune.objective import (
    ObjectiveComparison,
    ObjectivePolicy,
    ObjectiveTrial,
    compare_candidate,
    objective_trial_from_summary,
    select_historical_best,
)
from pipetune.runner import MeasureRequest, measure
from pipetune.session import ConfigPair, SessionState, TuningSessionStore


class ControllerError(RuntimeError):
    """Raised when a tuning round cannot preserve its lifecycle contract."""


class TrialExecutor(Protocol):
    def execute(
        self,
        *,
        trial_id: str,
        target_config: pathlib.Path,
        peer_config: pathlib.Path,
        destination: pathlib.Path,
    ) -> SteadySummary: ...


@dataclasses.dataclass(frozen=True)
class TrialObservation:
    trial_id: str
    target_config: pathlib.Path
    peer_config: pathlib.Path
    summary: SteadySummary
    objective: ObjectiveTrial


@dataclasses.dataclass(frozen=True)
class CandidateObservation:
    candidate: Candidate
    trial: TrialObservation
    comparison: ObjectiveComparison
    reused_probe: bool


@dataclasses.dataclass(frozen=True)
class RoundResult:
    state: SessionState
    previous_accepted: ConfigPair
    diagnosis: Diagnosis
    baseline_trial_id: str
    probe_trial_id: str | None
    candidate_trials: tuple[str, ...]
    accepted_trial_id: str | None
    reused_probe: bool
    comparisons: tuple[ObjectiveComparison, ...]


class MeasureTrialExecutor:
    """Publish one E8 measure result as an immutable tuning trial subtree."""

    def __init__(
        self,
        *,
        request: MeasureRequest,
        measure_function: Callable[..., object] = measure,
        config_tool: object | None = None,
        transport_factory: Callable[..., object] | None = None,
        sleeper: Callable[[float], None] | None = None,
    ) -> None:
        self._request = request
        self._measure = measure_function
        self._config_tool = config_tool
        self._transport_factory = transport_factory
        self._sleeper = sleeper

    def execute(
        self,
        *,
        trial_id: str,
        target_config: pathlib.Path,
        peer_config: pathlib.Path,
        destination: pathlib.Path,
    ) -> SteadySummary:
        if destination.exists():
            raise ControllerError(f"trial destination already exists: {destination}")
        tuning_root = destination.parent.parent
        tuning_root.mkdir(parents=True, exist_ok=True)
        stage = pathlib.Path(
            tempfile.mkdtemp(prefix=f".{trial_id}.", dir=tuning_root)
        )
        output = stage / "runner"
        try:
            request = dataclasses.replace(
                self._request,
                target_config=target_config,
                peer_config=peer_config,
                output=output,
            )
            options: dict[str, object] = {
                "trial_id_factory": lambda: trial_id,
            }
            if self._config_tool is not None:
                options["config_tool"] = self._config_tool
            if self._transport_factory is not None:
                options["transport_factory"] = self._transport_factory
            if self._sleeper is not None:
                options["sleeper"] = self._sleeper
            result = self._measure(request, **options)
            manifest = getattr(result, "manifest", None)
            expected = pathlib.PurePosixPath("trials") / trial_id / "trial.json"
            if manifest is None or pathlib.PurePosixPath(manifest.path) != expected:
                raise ControllerError("runner published an unexpected trial manifest")
            source = output / pathlib.PurePosixPath(manifest.path).parent
            if not source.is_dir() or not (source / "trial.json").is_file():
                raise ControllerError("runner trial subtree is incomplete")
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.replace(source, destination)
            if hasattr(os, "O_DIRECTORY"):
                descriptor = os.open(
                    destination.parent, os.O_RDONLY | os.O_DIRECTORY
                )
                try:
                    os.fsync(descriptor)
                finally:
                    os.close(descriptor)
            return summarize_trial(destination / "trial.json")
        finally:
            shutil.rmtree(stage, ignore_errors=True)


class ColdStartController:
    """Run one complete diagnosis/probe/candidate/selection round."""

    def __init__(
        self,
        *,
        root: pathlib.Path,
        store: TuningSessionStore,
        config_tool: CandidateConfigTool,
        executor: TrialExecutor,
        policy: ObjectivePolicy,
        candidate_generator: Callable[..., tuple[Candidate, ...]] = generate_candidates,
        diagnoser: Callable[..., Diagnosis] = diagnose_summary,
        objective_factory: Callable[[SteadySummary], ObjectiveTrial] = (
            objective_trial_from_summary
        ),
        trial_id_factory: Callable[[str], str] | None = None,
    ) -> None:
        self._root = root.resolve()
        self._store = store
        self._config_tool = config_tool
        self._executor = executor
        self._policy = policy
        self._candidate_generator = candidate_generator
        self._diagnoser = diagnoser
        self._objective_factory = objective_factory
        self._trial_id_factory = trial_id_factory or (
            lambda purpose: f"{purpose}-{uuid.uuid4().hex}"
        )

    def _path(self, relative: str) -> pathlib.Path:
        path = (self._root / pathlib.PurePosixPath(relative)).resolve()
        try:
            path.relative_to(self._root)
        except ValueError as error:
            raise ControllerError(f"session path escapes root: {relative}") from error
        return path

    def _run_trial(
        self,
        state: SessionState,
        *,
        purpose: str,
        target_config: pathlib.Path,
        peer_config: pathlib.Path,
    ) -> tuple[SessionState, TrialObservation]:
        trial_id = self._trial_id_factory(purpose)
        running = self._store.start_trial(state, trial_id)
        destination = self._root / "trials" / trial_id
        summary = self._executor.execute(
            trial_id=trial_id,
            target_config=target_config,
            peer_config=peer_config,
            destination=destination,
        )
        if summary.trial_id != trial_id:
            raise ControllerError("trial executor returned a mismatched trial ID")
        recovery = self._store.resume(running.identity)
        if recovery.action != "finalized" or recovery.trial_id != trial_id:
            raise ControllerError("completed trial was not atomically finalized")
        return recovery.state, TrialObservation(
            trial_id=trial_id,
            target_config=target_config,
            peer_config=peer_config,
            summary=summary,
            objective=self._objective_factory(summary),
        )

    def _generate(
        self,
        diagnosis: Diagnosis,
        *,
        target_config: pathlib.Path,
        peer_config: pathlib.Path,
        output_dir: pathlib.Path,
    ) -> tuple[Candidate, ...]:
        return self._candidate_generator(
            diagnosis,
            target_config=target_config,
            peer_config=peer_config,
            output_dir=output_dir,
            config_tool=self._config_tool,
        )

    @staticmethod
    def _details(round_index: int, **values: object) -> dict[str, object]:
        return {"round": round_index, **values}

    @staticmethod
    def _same_config_pair(left: Candidate, right: Candidate) -> bool:
        try:
            return (
                left.target_config.read_bytes() == right.target_config.read_bytes()
                and left.peer_config.read_bytes() == right.peer_config.read_bytes()
            )
        except OSError as error:
            raise ControllerError(f"cannot compare probe candidate bytes: {error}") from error

    def run_round(
        self, state: SessionState, *, round_index: int
    ) -> RoundResult:
        if state.phase not in ("baseline", "accepted", "rolled_back"):
            raise ControllerError(
                f"controller requires a round boundary, got {state.phase}"
            )
        if type(round_index) is not int or round_index < 1:
            raise ControllerError("round index must be a positive integer")

        previous_accepted = state.accepted
        accepted_target = self._path(previous_accepted.target.path)
        accepted_peer = self._path(previous_accepted.peer.path)
        state, baseline = self._run_trial(
            state,
            purpose=f"round-{round_index:02d}-baseline",
            target_config=accepted_target,
            peer_config=accepted_peer,
        )
        diagnosis = self._diagnoser(baseline.summary)
        state = self._store.transition(
            state,
            phase="diagnose",
            details=self._details(
                round_index,
                baseline_trial_id=baseline.trial_id,
                diagnosis=diagnosis.point,
            ),
        )

        probe: TrialObservation | None = None
        probe_candidate: Candidate | None = None
        round_root = self._root / "configs" / f"round-{round_index:04d}"
        if diagnosis.point == "probe_required":
            probes = self._generate(
                diagnosis,
                target_config=accepted_target,
                peer_config=accepted_peer,
                output_dir=round_root / "probe",
            )
            if len(probes) != 1:
                raise ControllerError("required diagnosis must produce one C1 probe")
            probe_candidate = probes[0]
            specification = diagnosis.required_probe
            expected_overrides = (
                ((specification.knob, specification.candidate_value),)
                if specification is not None
                else ()
            )
            if (
                specification is None
                or probe_candidate.action.name != "c1-probe"
                or probe_candidate.action.kind != "c1"
                or probe_candidate.action.overrides != expected_overrides
            ):
                raise ControllerError("required diagnosis produced an invalid C1 probe")
            state = self._store.transition(
                state,
                phase="probe",
                details=self._details(
                    round_index,
                    baseline_trial_id=baseline.trial_id,
                    probe_candidate_id=probe_candidate.candidate_id,
                ),
            )
            state, probe = self._run_trial(
                state,
                purpose=f"round-{round_index:02d}-probe",
                target_config=probe_candidate.target_config,
                peer_config=probe_candidate.peer_config,
            )
            diagnosis = self._diagnoser(
                baseline.summary,
                probe_summary=probe.summary,
            )
            state = self._store.transition(
                state,
                phase="diagnose",
                details=self._details(
                    round_index,
                    baseline_trial_id=baseline.trial_id,
                    probe_trial_id=probe.trial_id,
                    diagnosis=diagnosis.point,
                ),
            )

        candidates = self._generate(
            diagnosis,
            target_config=accepted_target,
            peer_config=accepted_peer,
            output_dir=round_root / "candidates",
        )
        state = self._store.transition(
            state,
            phase="candidates",
            details=self._details(
                round_index,
                candidate_ids=[item.candidate_id for item in candidates],
                diagnosis=diagnosis.point,
            ),
        )

        observations: list[tuple[Candidate, TrialObservation, bool]] = []
        for candidate in candidates:
            completed_probe = diagnosis.completed_probe
            reuse = (
                probe is not None
                and probe_candidate is not None
                and diagnosis.point == "P4"
                and candidate.action.name == "c1-increase"
                and candidate.action.kind == "c1"
                and completed_probe is not None
                and completed_probe.direction == 1
                and self._same_config_pair(candidate, probe_candidate)
            )
            if reuse:
                observation = probe
            else:
                state, observation = self._run_trial(
                    state,
                    purpose=f"round-{round_index:02d}-{candidate.action.name}",
                    target_config=candidate.target_config,
                    peer_config=candidate.peer_config,
                )
            observations.append((candidate, observation, reuse))

        state = self._store.transition(
            state,
            phase="select",
            details=self._details(
                round_index,
                candidate_trials=[item[1].trial_id for item in observations],
            ),
        )
        evaluated = tuple(
            CandidateObservation(
                candidate=candidate,
                trial=observation,
                comparison=compare_candidate(
                    baseline.objective,
                    observation.objective,
                    self._policy,
                ),
                reused_probe=reuse,
            )
            for candidate, observation, reuse in observations
        )
        improving = tuple(
            item.trial.objective for item in evaluated if item.comparison.accepted
        )
        best = select_historical_best(
            baseline=baseline.objective,
            accepted_trials=improving,
            policy=self._policy,
        )
        selected = next(
            (item for item in evaluated if item.trial.trial_id == best.trial_id),
            None,
        )
        if selected is None:
            state = self._store.transition(
                state,
                phase="rolled_back",
                details=self._details(
                    round_index,
                    baseline_trial_id=baseline.trial_id,
                    reason=(
                        "no legal candidate"
                        if not candidates
                        else "no significant improvement"
                    ),
                ),
            )
            accepted_trial_id = None
        else:
            pair = ConfigPair(
                target=artifact_ref(self._root, selected.candidate.target_config),
                peer=artifact_ref(self._root, selected.candidate.peer_config),
            )
            state = self._store.transition(
                state,
                phase="accepted",
                accepted=pair,
                details=self._details(
                    round_index,
                    accepted_candidate_id=selected.candidate.candidate_id,
                    accepted_trial_id=selected.trial.trial_id,
                    reused_probe=selected.reused_probe,
                ),
            )
            accepted_trial_id = selected.trial.trial_id

        return RoundResult(
            state=state,
            previous_accepted=previous_accepted,
            diagnosis=diagnosis,
            baseline_trial_id=baseline.trial_id,
            probe_trial_id=probe.trial_id if probe else None,
            candidate_trials=tuple(item.trial.trial_id for item in evaluated),
            accepted_trial_id=accepted_trial_id,
            reused_probe=any(item.reused_probe for item in evaluated),
            comparisons=tuple(item.comparison for item in evaluated),
        )


__all__ = [
    "ColdStartController",
    "ControllerError",
    "MeasureTrialExecutor",
    "RoundResult",
    "TrialExecutor",
]
