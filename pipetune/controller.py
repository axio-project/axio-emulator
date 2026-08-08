"""Cold-start diagnosis and candidate orchestration for PipeTune."""

from __future__ import annotations

import dataclasses
import math
import os
import pathlib
import re
import shutil
import tempfile
import uuid
from typing import Any, Callable, Protocol

from pipetune.artifacts import artifact_ref
from pipetune.candidates import Candidate, CandidateConfigTool, generate_candidates
from pipetune.diagnosis import (
    Diagnosis,
    Statistic,
    SteadySummary,
    diagnose_summary,
    diagnosis_document,
    summarize_trial,
)
from pipetune.impact import ExpectedImpactComparison, compare_expected_impact
from pipetune.model import ArtifactRef, ContractError
from pipetune.objective import (
    ObjectiveComparison,
    ObjectivePolicy,
    ObjectiveTrial,
    compare_candidate,
    objective_trial_from_summary,
    select_historical_best,
)
from pipetune.runner import MeasureError, MeasureRequest, measure
from pipetune.search_policy import (
    ComputeBottleneck,
    ImpactSpec,
    SearchAction,
    compute_actions,
    detect_compute_bottleneck,
)
from pipetune.session import ConfigPair, SessionState, TuningSessionStore
from pipetune.topology_candidates import materialize_actions
from pipetune.topology_state import TopologyState, TopologyStateError


class ControllerError(RuntimeError):
    """Raised when a tuning round cannot preserve its lifecycle contract."""


class TrialExecutionError(ControllerError):
    """A cold-start trial failed before publishing an objective result."""


class InfrastructureFailureLimit(ControllerError):
    """The configured number of consecutive trial failures was reached."""

    def __init__(
        self,
        *,
        state: SessionState,
        consecutive_failures: int,
        reason: str,
    ) -> None:
        super().__init__(reason)
        self.state = state
        self.consecutive_failures = consecutive_failures
        self.reason = reason


class InvalidControlEvidence(ControllerError):
    """A baseline or required probe is structurally unusable."""

    def __init__(self, *, state: SessionState, reason: str) -> None:
        super().__init__(reason)
        self.state = state
        self.reason = reason


class TrialExecutor(Protocol):
    def execute(
        self,
        *,
        trial_id: str,
        target_config: pathlib.Path,
        peer_config: pathlib.Path,
        destination: pathlib.Path,
    ) -> SteadySummary: ...


_CANDIDATE_PURPOSE = re.compile(
    r"^round-\d+(?:-restart-\d+)?-(?P<action>.+)$"
)


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
    expected_impact: ExpectedImpactComparison
    reused_probe: bool
    reused_visited: bool
    source_topology: TopologyState
    candidate_topology: TopologyState


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
    expected_impacts: tuple[ExpectedImpactComparison, ...]
    baseline_objective: ObjectiveTrial
    accepted_objective: ObjectiveTrial | None


@dataclasses.dataclass(frozen=True)
class ConvergenceResult:
    state: SessionState
    rounds: tuple[RoundResult, ...]
    stop_reason: str
    completed_rounds: int
    infrastructure_failures: int
    best: ConfigPair
    best_trial_id: str | None


@dataclasses.dataclass(frozen=True)
class ConvergenceSnapshot:
    completed_rounds: int
    stop_reason: str | None
    infrastructure_failures: int
    best: ConfigPair
    best_trial_id: str | None
    best_objective: ObjectiveTrial | None


@dataclasses.dataclass(frozen=True)
class RoundRecovery:
    state: SessionState
    round_index: int
    next_attempt: int


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
            try:
                result = self._measure(request, **options)
            except (MeasureError, OSError) as error:
                raise TrialExecutionError(
                    f"cold-start trial failed: {error}"
                ) from error
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
        action_materializer: Callable[..., tuple[Candidate, ...]] = materialize_actions,
        compute_bottleneck_factory: Callable[
            [SteadySummary], ComputeBottleneck | None
        ] = detect_compute_bottleneck,
        compute_action_factory: Callable[..., tuple[SearchAction, ...]] = compute_actions,
        diagnoser: Callable[..., Diagnosis] = diagnose_summary,
        diagnosis_serializer: Callable[..., dict[str, object]] = diagnosis_document,
        objective_factory: Callable[[SteadySummary], ObjectiveTrial] = (
            objective_trial_from_summary
        ),
        impact_comparer: Callable[..., ExpectedImpactComparison] = (
            compare_expected_impact
        ),
        trial_id_factory: Callable[[str], str] | None = None,
        infrastructure_failure_limit: int = 2,
    ) -> None:
        if (
            type(infrastructure_failure_limit) is not int
            or infrastructure_failure_limit < 1
        ):
            raise ControllerError(
                "infrastructure failure limit must be a positive integer"
            )
        self._root = root.resolve()
        self._store = store
        self._config_tool = config_tool
        self._executor = executor
        self._policy = policy
        self._candidate_generator = candidate_generator
        self._action_materializer = action_materializer
        self._compute_bottleneck_factory = compute_bottleneck_factory
        self._compute_action_factory = compute_action_factory
        self._diagnoser = diagnoser
        self._diagnosis_serializer = diagnosis_serializer
        self._objective_factory = objective_factory
        self._impact_comparer = impact_comparer
        self._trial_id_factory = trial_id_factory or (
            lambda purpose: f"{purpose}-{uuid.uuid4().hex}"
        )
        self._infrastructure_failure_limit = infrastructure_failure_limit

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
        round_index: int,
        round_attempt: int,
        purpose: str,
        target_config: pathlib.Path,
        peer_config: pathlib.Path,
        failure_key: str | None = None,
    ) -> tuple[SessionState, TrialObservation]:
        details = dict(state.details)
        details.pop("round_boundary", None)
        details.pop("round_recovery", None)
        details["round"] = round_index
        details["round_attempt"] = round_attempt
        details["active_trial"] = {
            "round_index": round_index,
            "round_attempt": round_attempt,
            "purpose": purpose,
            "failure_key": failure_key,
        }
        state = self._store.checkpoint(state, details=details)
        trial_id = self._trial_id_factory(purpose)
        already_finalized = False
        while True:
            running = self._store.start_trial(state, trial_id)
            destination = self._root / "trials" / trial_id
            try:
                summary = self._executor.execute(
                    trial_id=trial_id,
                    target_config=target_config,
                    peer_config=peer_config,
                    destination=destination,
                )
            except TrialExecutionError as error:
                manifest = destination / "trial.json"
                if manifest.is_file():
                    recovery = self._store.resume(running.identity)
                    if (
                        recovery.action != "finalized"
                        or recovery.trial_id != trial_id
                    ):
                        raise ControllerError(
                            "published failed trial was not finalized"
                        ) from error
                    state = recovery.state
                    summary = summarize_trial(manifest)
                    already_finalized = True
                    break
                consecutive = (
                    self._required_health_failures(running, failure_key) + 1
                    if failure_key is not None
                    else self._trailing_failures(running) + 1
                )
                failure_details = dict(running.details)
                if failure_key is not None:
                    failure_details[failure_key] = consecutive
                if consecutive >= self._infrastructure_failure_limit:
                    stopped = self._store.abandon_active_trial(
                        running,
                        details=failure_details,
                    )
                    raise InfrastructureFailureLimit(
                        state=stopped,
                        consecutive_failures=consecutive,
                        reason=str(error),
                    ) from error
                retry_id = self._trial_id_factory(
                    f"{purpose}-retry-{consecutive:02d}"
                )
                recovery = self._store.resume(
                    running.identity,
                    retry_trial_id=retry_id,
                    details=failure_details,
                )
                if recovery.action != "rerun" or recovery.trial_id != retry_id:
                    raise ControllerError(
                        "failed trial did not produce the requested retry"
                    ) from error
                state = recovery.state
                trial_id = retry_id
                continue
            break
        if summary.trial_id != trial_id:
            raise ControllerError("trial executor returned a mismatched trial ID")
        if already_finalized:
            finalized_state = state
        else:
            recovery = self._store.resume(running.identity)
            if recovery.action != "finalized" or recovery.trial_id != trial_id:
                raise ControllerError("completed trial was not atomically finalized")
            finalized_state = recovery.state
        return finalized_state, TrialObservation(
            trial_id=trial_id,
            target_config=target_config,
            peer_config=peer_config,
            summary=summary,
            objective=self._objective_factory(summary),
        )

    @staticmethod
    def _trailing_failures(state: SessionState) -> int:
        failures = 0
        for attempt in reversed(state.attempts):
            if attempt.status in ("pending", "running"):
                continue
            if attempt.status == "abandoned":
                failures += 1
                continue
            if attempt.status == "complete":
                break
        return failures

    @staticmethod
    def _required_health_failures(state: SessionState, key: str) -> int:
        value = state.details.get(key, 0)
        if type(value) is not int or value < 0:
            raise ControllerError(f"{key} is invalid")
        return value

    def _run_required_observation(
        self,
        state: SessionState,
        *,
        round_index: int,
        round_attempt: int,
        purpose: str,
        target_config: pathlib.Path,
        peer_config: pathlib.Path,
        failure_key: str,
        label: str,
    ) -> tuple[SessionState, TrialObservation]:
        failures = self._required_health_failures(state, failure_key)
        current_purpose = purpose
        while True:
            state, observation = self._run_trial(
                state,
                round_index=round_index,
                round_attempt=round_attempt,
                purpose=current_purpose,
                target_config=target_config,
                peer_config=peer_config,
                failure_key=failure_key,
            )
            failures = self._required_health_failures(state, failure_key)
            if observation.objective.status == "valid":
                return state, observation
            if observation.objective.status == "invalid":
                reason = (
                    f"{label} objective is structurally invalid: "
                    f"{observation.objective.rejection_reason}"
                )
                details = dict(state.details)
                details.pop("active_trial", None)
                details["invalid_control_evidence"] = {
                    "label": label,
                    "trial_id": observation.trial_id,
                    "reason": reason,
                }
                state = self._store.checkpoint(state, details=details)
                raise InvalidControlEvidence(
                    state=state,
                    reason=reason,
                )

            failures += 1
            current_purpose = f"{purpose}-health-retry-{failures:02d}"
            details = dict(state.details)
            details[failure_key] = failures
            details["active_trial"] = {
                "round_index": round_index,
                "round_attempt": round_attempt,
                "purpose": current_purpose,
                "failure_key": failure_key,
            }
            state = self._store.checkpoint(state, details=details)
            if failures >= self._infrastructure_failure_limit:
                raise InfrastructureFailureLimit(
                    state=state,
                    consecutive_failures=failures,
                    reason=(
                        f"{label} remained unhealthy: "
                        f"{observation.objective.rejection_reason}"
                    ),
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
    def _details(
        state: SessionState,
        round_index: int,
        **values: object,
    ) -> dict[str, object]:
        details: dict[str, object] = {"round": round_index, **values}
        round_attempt = state.details.get("round_attempt")
        if type(round_attempt) is int and round_attempt > 0:
            details["round_attempt"] = round_attempt
        convergence = state.details.get("convergence")
        if isinstance(convergence, dict):
            details["convergence"] = convergence
        bootstrap = state.details.get("bootstrap")
        if isinstance(bootstrap, dict):
            details["bootstrap"] = bootstrap
        visited = state.details.get("visited_candidates")
        if "visited_candidates" not in values and isinstance(visited, list):
            details["visited_candidates"] = visited
        recovered = state.details.get("recovered_candidate_trials")
        if "recovered_candidate_trials" not in values and isinstance(
            recovered, dict
        ):
            details["recovered_candidate_trials"] = recovered
        return details

    @staticmethod
    def _same_config_pair(left: Candidate, right: Candidate) -> bool:
        try:
            return (
                left.target_config.read_bytes() == right.target_config.read_bytes()
                and left.peer_config.read_bytes() == right.peer_config.read_bytes()
            )
        except OSError as error:
            raise ControllerError(f"cannot compare probe candidate bytes: {error}") from error

    def _materialize_search_actions(
        self,
        actions: tuple[SearchAction, ...],
        *,
        target_config: pathlib.Path,
        peer_config: pathlib.Path,
        output_dir: pathlib.Path,
    ) -> tuple[Candidate, ...]:
        return self._action_materializer(
            actions,
            target_config=target_config,
            peer_config=peer_config,
            output_dir=output_dir,
            config_tool=self._config_tool,
        )

    @staticmethod
    def _topology(summary: SteadySummary) -> TopologyState:
        try:
            return TopologyState.from_config(summary.canonical_target)
        except TopologyStateError as error:
            raise ControllerError(f"trial has invalid canonical topology: {error}") from error

    @staticmethod
    def _visited_trial_id(state: SessionState, candidate: Candidate) -> str | None:
        records = state.details.get("visited_candidates", [])
        if not isinstance(records, list):
            raise ControllerError("visited candidate evidence must be an array")
        for record in records:
            if not isinstance(record, dict):
                raise ControllerError("visited candidate evidence must contain objects")
            if (
                record.get("target_sha256") == candidate.target_sha256
                and record.get("peer_sha256") == candidate.peer_sha256
            ):
                trial_id = record.get("trial_id")
                if not isinstance(trial_id, str) or not trial_id:
                    raise ControllerError("visited candidate has no trial ID")
                return trial_id
        return None

    @staticmethod
    def _recovered_trial_id(
        state: SessionState, candidate: Candidate
    ) -> str | None:
        records = state.details.get("recovered_candidate_trials", {})
        if not isinstance(records, dict):
            raise ControllerError("recovered candidate evidence must be an object")
        trial_id = records.get(candidate.action.name)
        if trial_id is None:
            return None
        if not isinstance(trial_id, str) or not trial_id:
            raise ControllerError("recovered candidate has no trial ID")
        return trial_id

    @staticmethod
    def _select_candidate(
        observations: tuple[CandidateObservation, ...],
        policy: ObjectivePolicy,
    ) -> CandidateObservation | None:
        valid = tuple(
            sorted(
                (
                    item
                    for item in observations
                    if item.expected_impact.accepted and item.comparison.accepted
                ),
                key=lambda item: (
                    item.candidate.action.name,
                    item.candidate.target_sha256,
                    item.candidate.peer_sha256,
                ),
            )
        )
        if not valid:
            return None
        best = select_historical_best(
            baseline=valid[0].trial.objective,
            accepted_trials=(item.trial.objective for item in valid[1:]),
            policy=policy,
        )
        return next(item for item in valid if item.trial.trial_id == best.trial_id)

    def _evaluate_candidates(
        self,
        state: SessionState,
        *,
        baseline: TrialObservation,
        diagnosis: Diagnosis,
        candidates: tuple[Candidate, ...],
        phase: str,
        round_index: int,
        round_attempt: int,
        round_label: str,
        probe: TrialObservation | None,
        probe_candidate: Candidate | None,
        evaluation_documents: list[dict[str, object]],
    ) -> tuple[SessionState, tuple[CandidateObservation, ...]]:
        source_topology = self._topology(baseline.summary)
        evaluated: list[CandidateObservation] = []
        for candidate in candidates:
            completed_probe = diagnosis.completed_probe
            reuse_probe = (
                probe is not None
                and probe_candidate is not None
                and diagnosis.point == "P4"
                and candidate.action.name == "c1-increase"
                and candidate.action.kind == "c1"
                and completed_probe is not None
                and completed_probe.direction == 1
                and self._same_config_pair(candidate, probe_candidate)
            )
            visited_trial_id = self._visited_trial_id(state, candidate)
            recovered_trial_id = self._recovered_trial_id(state, candidate)
            if visited_trial_id is None:
                visited_trial_id = recovered_trial_id
            reuse_visited = visited_trial_id is not None
            if reuse_probe:
                observation = probe
            elif visited_trial_id is not None:
                manifest = self._root / "trials" / visited_trial_id / "trial.json"
                if not manifest.is_file():
                    raise ControllerError("visited candidate trial manifest is missing")
                summary = summarize_trial(manifest)
                observation = TrialObservation(
                    trial_id=visited_trial_id,
                    target_config=candidate.target_config,
                    peer_config=candidate.peer_config,
                    summary=summary,
                    objective=self._objective_factory(summary),
                )
            else:
                state, observation = self._run_trial(
                    state,
                    round_index=round_index,
                    round_attempt=round_attempt,
                    purpose=f"{round_label}-{candidate.action.name}",
                    target_config=candidate.target_config,
                    peer_config=candidate.peer_config,
                )

            candidate_topology = self._topology(observation.summary)
            impact: Diagnosis | ImpactSpec = candidate.action.impact
            if impact.kind == "diagnosis" and impact.metric is None:
                impact = diagnosis
            comparison = compare_candidate(
                baseline.objective,
                observation.objective,
                self._policy,
                allow_equivalent_resource_reduction=(
                    candidate.action.allow_equivalent_resource_reduction
                ),
                accepted_physical_cores=source_topology.physical_core_count,
                candidate_physical_cores=candidate_topology.physical_core_count,
            )
            expected_impact = self._impact_comparer(
                impact,
                baseline.summary,
                observation.summary,
                candidate_id=observation.trial_id,
            )
            item = CandidateObservation(
                candidate=candidate,
                trial=observation,
                comparison=comparison,
                expected_impact=expected_impact,
                reused_probe=reuse_probe,
                reused_visited=reuse_visited,
                source_topology=source_topology,
                candidate_topology=candidate_topology,
            )
            if item.expected_impact.candidate_id != item.trial.trial_id:
                raise ControllerError(
                    "expected-impact comparison has a mismatched trial ID"
                )
            document = _candidate_evaluation_document(item)
            evaluation_documents.append(document)
            if not reuse_visited:
                visited = list(state.details.get("visited_candidates", []))
                visited.append(document)
            else:
                visited = list(state.details.get("visited_candidates", []))
                if recovered_trial_id is not None:
                    visited.append(document)
            recovered = dict(state.details.get("recovered_candidate_trials", {}))
            if recovered_trial_id is not None:
                recovered.pop(candidate.action.name, None)
            state = self._store.checkpoint(
                state,
                details=self._details(
                    state,
                    round_index,
                    search_phase=phase,
                    candidate_evaluations=evaluation_documents,
                    visited_candidates=visited,
                    recovered_candidate_trials=recovered,
                ),
            )
            evaluated.append(item)
        return state, tuple(evaluated)

    def _evaluate_actions(
        self,
        state: SessionState,
        *,
        baseline: TrialObservation,
        diagnosis: Diagnosis,
        actions: tuple[SearchAction, ...],
        target_config: pathlib.Path,
        peer_config: pathlib.Path,
        output_dir: pathlib.Path,
        round_index: int,
        round_attempt: int,
        round_label: str,
        evaluation_documents: list[dict[str, object]],
    ) -> tuple[
        SessionState,
        tuple[Candidate, ...],
        tuple[CandidateObservation, ...],
    ]:
        candidates = self._materialize_search_actions(
            actions,
            target_config=target_config,
            peer_config=peer_config,
            output_dir=output_dir,
        )
        state, observations = self._evaluate_candidates(
            state,
            baseline=baseline,
            diagnosis=diagnosis,
            candidates=candidates,
            phase="compute",
            round_index=round_index,
            round_attempt=round_attempt,
            round_label=round_label,
            probe=None,
            probe_candidate=None,
            evaluation_documents=evaluation_documents,
        )
        return state, candidates, observations

    def run_round(
        self,
        state: SessionState,
        *,
        round_index: int,
        round_attempt: int = 1,
    ) -> RoundResult:
        if state.phase not in ("baseline", "accepted", "rolled_back"):
            raise ControllerError(
                f"controller requires a round boundary, got {state.phase}"
            )
        if type(round_index) is not int or round_index < 1:
            raise ControllerError("round index must be a positive integer")
        if type(round_attempt) is not int or round_attempt < 1:
            raise ControllerError("round attempt must be a positive integer")

        round_label = f"round-{round_index:02d}"
        round_directory = f"round-{round_index:04d}"
        if round_attempt > 1:
            round_label += f"-restart-{round_attempt:02d}"
            round_directory += f"-restart-{round_attempt:02d}"

        previous_accepted = state.accepted
        accepted_target = self._path(previous_accepted.target.path)
        accepted_peer = self._path(previous_accepted.peer.path)
        state, baseline = self._run_required_observation(
            state,
            round_index=round_index,
            round_attempt=round_attempt,
            purpose=f"{round_label}-baseline",
            target_config=accepted_target,
            peer_config=accepted_peer,
            failure_key="baseline_health_failures",
            label="baseline",
        )
        diagnosis = self._diagnoser(baseline.summary)
        persisted_diagnosis = self._diagnosis_serializer(
            diagnosis,
            baseline.summary,
        )
        state = self._store.transition(
            state,
            phase="diagnose",
            details=self._details(
                state,
                round_index,
                baseline_trial_id=baseline.trial_id,
                diagnosis=diagnosis.point,
                diagnosis_document=persisted_diagnosis,
            ),
        )

        probe: TrialObservation | None = None
        probe_candidate: Candidate | None = None
        round_root = self._root / "configs" / round_directory
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
                    state,
                    round_index,
                    baseline_trial_id=baseline.trial_id,
                    probe_candidate_id=probe_candidate.candidate_id,
                ),
            )
            state, probe = self._run_required_observation(
                state,
                round_index=round_index,
                round_attempt=round_attempt,
                purpose=f"{round_label}-probe",
                target_config=probe_candidate.target_config,
                peer_config=probe_candidate.peer_config,
                failure_key="probe_health_failures",
                label="required probe",
            )
            diagnosis = self._diagnoser(
                baseline.summary,
                probe_summary=probe.summary,
            )
            persisted_diagnosis = self._diagnosis_serializer(
                diagnosis,
                baseline.summary,
                probe_summary=probe.summary,
            )
            state = self._store.transition(
                state,
                phase="diagnose",
                details=self._details(
                    state,
                    round_index,
                    baseline_trial_id=baseline.trial_id,
                    probe_trial_id=probe.trial_id,
                    diagnosis=diagnosis.point,
                    diagnosis_document=persisted_diagnosis,
                ),
            )

        memory_candidates = self._generate(
            diagnosis,
            target_config=accepted_target,
            peer_config=accepted_peer,
            output_dir=round_root / "candidates",
        )
        state = self._store.transition(
            state,
            phase="candidates",
            details=self._details(
                state,
                round_index,
                search_phase="memory",
                candidate_ids=[item.candidate_id for item in memory_candidates],
                diagnosis=diagnosis.point,
                diagnosis_document=persisted_diagnosis,
            ),
        )
        evaluation_documents: list[dict[str, object]] = []
        state, memory_evaluated = self._evaluate_candidates(
            state,
            baseline=baseline,
            diagnosis=diagnosis,
            candidates=memory_candidates,
            phase="memory",
            round_index=round_index,
            round_attempt=round_attempt,
            round_label=round_label,
            probe=probe,
            probe_candidate=probe_candidate,
            evaluation_documents=evaluation_documents,
        )
        selected = self._select_candidate(memory_evaluated, self._policy)
        compute_candidates: tuple[Candidate, ...] = ()
        compute_evaluated: tuple[CandidateObservation, ...] = ()
        rollback_reason: str | None = None
        if selected is None:
            bottleneck = self._compute_bottleneck_factory(baseline.summary)
            if bottleneck is None:
                rollback_reason = (
                    "memory candidates exhausted without compute-bound evidence"
                )
            else:
                try:
                    runtime = baseline.summary.canonical_target["knobs"]["runtime"]
                except (KeyError, TypeError) as error:
                    raise ControllerError(
                        "baseline has no runtime knobs for compute search"
                    ) from error
                if not isinstance(runtime, dict):
                    raise ControllerError(
                        "baseline runtime knobs must be an object"
                    )
                actions = self._compute_action_factory(
                    bottleneck,
                    self._topology(baseline.summary),
                    runtime,
                )
                state = self._store.checkpoint(
                    state,
                    details=self._details(
                        state,
                        round_index,
                        search_phase="compute",
                        compute_bottleneck={
                            "role": bottleneck.role,
                            "metric": bottleneck.metric,
                        },
                        candidate_evaluations=evaluation_documents,
                    ),
                )
                state, compute_candidates, compute_evaluated = self._evaluate_actions(
                    state,
                    baseline=baseline,
                    diagnosis=diagnosis,
                    actions=actions,
                    target_config=accepted_target,
                    peer_config=accepted_peer,
                    output_dir=round_root / "compute",
                    round_index=round_index,
                    round_attempt=round_attempt,
                    round_label=round_label,
                    evaluation_documents=evaluation_documents,
                )
                selected = self._select_candidate(compute_evaluated, self._policy)
                if selected is None:
                    rollback_reason = (
                        "no legal candidate"
                        if not compute_candidates
                        else "all candidates invalid"
                    )

        evaluated = (*memory_evaluated, *compute_evaluated)
        state = self._store.transition(
            state,
            phase="select",
            details=self._details(
                state,
                round_index,
                candidate_trials=[item.trial.trial_id for item in evaluated],
                candidate_evaluations=evaluation_documents,
                diagnosis_document=persisted_diagnosis,
            ),
        )
        if selected is None:
            if rollback_reason is None:
                raise ControllerError("candidate selection has no rollback reason")
            state = self._store.transition(
                state,
                phase="rolled_back",
                details=self._details(
                    state,
                    round_index,
                    baseline_trial_id=baseline.trial_id,
                    candidate_evaluations=evaluation_documents,
                    diagnosis_document=persisted_diagnosis,
                    reason=rollback_reason,
                    round_boundary=_round_boundary_document(
                        round_index=round_index,
                        previous_accepted=previous_accepted,
                        baseline_objective=baseline.objective,
                        accepted_objective=None,
                        accepted_trial_id=None,
                        rollback_reason=rollback_reason,
                    ),
                ),
            )
            accepted_trial_id = None
            accepted_objective = None
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
                    state,
                    round_index,
                    accepted_candidate_id=selected.candidate.candidate_id,
                    accepted_trial_id=selected.trial.trial_id,
                    accepted_search_phase=selected.candidate.action.phase.value,
                    candidate_evaluations=evaluation_documents,
                    diagnosis_document=persisted_diagnosis,
                    reused_probe=selected.reused_probe,
                    round_boundary=_round_boundary_document(
                        round_index=round_index,
                        previous_accepted=previous_accepted,
                        baseline_objective=baseline.objective,
                        accepted_objective=selected.trial.objective,
                        accepted_trial_id=selected.trial.trial_id,
                        rollback_reason=None,
                    ),
                ),
            )
            accepted_trial_id = selected.trial.trial_id
            accepted_objective = selected.trial.objective

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
            expected_impacts=tuple(item.expected_impact for item in evaluated),
            baseline_objective=baseline.objective,
            accepted_objective=accepted_objective,
        )

    @staticmethod
    def _active_trial_cursor(
        state: SessionState,
    ) -> tuple[int, int, str | None]:
        value = state.details.get("active_trial")
        if value is None:
            round_index = state.details.get("round")
            round_attempt = state.details.get("round_attempt", 1)
            failure_key = None
        else:
            if not isinstance(value, dict) or set(value) != {
                "round_index",
                "round_attempt",
                "purpose",
                "failure_key",
            }:
                raise ControllerError("active trial cursor is invalid")
            round_index = value["round_index"]
            round_attempt = value["round_attempt"]
            failure_key = value["failure_key"]
            if not isinstance(value["purpose"], str) or not value["purpose"]:
                raise ControllerError("active trial purpose is invalid")
            if failure_key not in (
                None,
                "baseline_health_failures",
                "probe_health_failures",
            ):
                raise ControllerError("active trial failure key is invalid")
        if type(round_index) is not int or round_index < 1:
            raise ControllerError("recovering round index is invalid")
        if type(round_attempt) is not int or round_attempt < 1:
            raise ControllerError("recovering round attempt is invalid")
        return round_index, round_attempt, failure_key

    def recover_round(self, state: SessionState) -> RoundRecovery:
        """Return an interrupted, uncounted round to its accepted boundary."""

        round_index, round_attempt, failure_key = self._active_trial_cursor(state)
        cursor = state.details.get("active_trial")
        purpose = cursor.get("purpose") if isinstance(cursor, dict) else None
        candidate_action = None
        if failure_key is None and isinstance(purpose, str):
            matched = _CANDIDATE_PURPOSE.fullmatch(purpose)
            if matched is not None:
                action = matched.group("action")
                if "baseline" not in action and action != "probe":
                    candidate_action = action
        active = tuple(
            attempt
            for attempt in state.attempts
            if attempt.status in ("pending", "running")
        )
        if active:
            if len(active) != 1:
                raise ControllerError("interrupted round has ambiguous active trials")
            attempt = active[0]
            manifest = self._root / attempt.manifest_path
            if attempt.status == "running" and manifest.is_file():
                recovery_details: dict[str, Any] | None = None
                objective: ObjectiveTrial | None = None
                if failure_key is not None:
                    summary = summarize_trial(manifest)
                    objective = self._objective_factory(summary)
                    recovery_details = dict(state.details)
                    if objective.status == "invalid":
                        reason = (
                            "required control objective is structurally invalid: "
                            f"{objective.rejection_reason}"
                        )
                        recovery_details.pop("active_trial", None)
                        recovery_details["invalid_control_evidence"] = {
                            "label": "required control",
                            "trial_id": attempt.trial_id,
                            "reason": reason,
                        }
                    elif objective.status != "valid":
                        recovery_details[failure_key] = (
                            self._required_health_failures(state, failure_key) + 1
                        )
                recovery = self._store.resume(
                    state.identity,
                    details=recovery_details,
                )
                if recovery.action != "finalized":
                    raise ControllerError("published interrupted trial was not finalized")
                state = recovery.state
                if candidate_action is not None:
                    recovered = dict(
                        state.details.get("recovered_candidate_trials", {})
                    )
                    recovered[candidate_action] = attempt.trial_id
                    state = self._store.checkpoint(
                        state,
                        details={
                            **state.details,
                            "recovered_candidate_trials": recovered,
                        },
                    )
                if objective is not None and objective.status == "invalid":
                    raise InvalidControlEvidence(
                        state=state,
                        reason=state.details["invalid_control_evidence"]["reason"],
                    )
            else:
                details = dict(state.details)
                if failure_key is not None and attempt.status == "running":
                    details[failure_key] = (
                        self._required_health_failures(state, failure_key) + 1
                    )
                state = self._store.abandon_active_trial(
                    state,
                    details=details,
                )

        consecutive = max(
            self._trailing_failures(state),
            self._required_health_failures(state, "baseline_health_failures"),
            self._required_health_failures(state, "probe_health_failures"),
        )
        if consecutive >= self._infrastructure_failure_limit:
            raise InfrastructureFailureLimit(
                state=state,
                consecutive_failures=consecutive,
                reason="interrupted trial reached the infrastructure failure limit",
            )

        details = dict(state.details)
        details.pop("active_trial", None)
        details.pop("round_recovery", None)
        details.pop("round_boundary", None)
        details["round"] = round_index
        details["round_attempt"] = round_attempt
        details["round_recovery"] = {
            "round_index": round_index,
            "next_attempt": round_attempt + 1,
        }
        if state.phase in ("baseline", "accepted", "rolled_back"):
            state = self._store.checkpoint(state, details=details)
        else:
            state = self._store.transition(
                state,
                phase="rolled_back",
                details=details,
            )
        return RoundRecovery(
            state=state,
            round_index=round_index,
            next_attempt=round_attempt + 1,
        )


class RoundRunner(Protocol):
    def run_round(
        self,
        state: SessionState,
        *,
        round_index: int,
        round_attempt: int = 1,
    ) -> RoundResult: ...

    def recover_round(self, state: SessionState) -> RoundRecovery: ...


def _statistic_document(value: object) -> dict[str, object]:
    statistic = value
    return {
        "samples": list(statistic.samples),
        "median": statistic.median,
        "mad": statistic.mad,
        "uncertainty": statistic.uncertainty,
        "unit": statistic.unit,
    }


def _objective_document(value: ObjectiveTrial) -> dict[str, object]:
    if value.status != "valid":
        raise ControllerError("historical best objective must be valid")
    return {
        "trial_id": value.trial_id,
        "status": value.status,
        "client_p999": _statistic_document(value.client_p999),
        "server_throughput": _statistic_document(value.server_throughput),
        "rejection_reason": value.rejection_reason,
    }


def _expected_impact_document(
    value: ExpectedImpactComparison,
) -> dict[str, object]:
    return {
        "candidate_id": value.candidate_id,
        "point": value.point,
        "metric": value.metric,
        "accepted": value.accepted,
        "reason": value.reason,
        "baseline_value": value.baseline_value,
        "candidate_value": value.candidate_value,
        "observed_reduction": value.observed_reduction,
        "required_reduction": value.required_reduction,
        "unit": value.unit,
    }


def _objective_comparison_document(
    value: ObjectiveComparison,
) -> dict[str, object]:
    return {
        "candidate_id": value.candidate_id,
        "accepted": value.accepted,
        "reason": value.reason,
        "metric": value.metric,
        "observed_improvement": value.observed_improvement,
        "required_improvement": value.required_improvement,
        "accepted_feasible": value.accepted_feasible,
        "candidate_feasible": value.candidate_feasible,
        "acceptance_mode": value.acceptance_mode,
        "physical_core_delta": value.physical_core_delta,
    }


def _topology_document(value: TopologyState) -> dict[str, object]:
    return {
        "application_count": value.application_count,
        "dispatcher_count": value.dispatcher_count,
        "overlap_count": value.overlap_count,
        "physical_core_count": value.physical_core_count,
        "physical_core_budget": value.physical_core_budget,
        "fanout": [
            {"dispatcher": dispatcher, "applications": applications}
            for dispatcher, applications in sorted(value.fanout_by_dispatcher.items())
        ],
    }


def _impact_spec_document(value: ImpactSpec) -> dict[str, object]:
    return {
        "kind": value.kind,
        "metric": value.metric,
        "direction": value.direction,
    }


def _candidate_evaluation_document(
    value: CandidateObservation,
) -> dict[str, object]:
    rejection_reason = None
    if not value.expected_impact.accepted:
        rejection_reason = value.expected_impact.reason
    elif not value.comparison.accepted:
        rejection_reason = value.comparison.reason
    return {
        "candidate_id": value.candidate.candidate_id,
        "action": value.candidate.action.name,
        "kind": value.candidate.action.kind,
        "profile": value.candidate.action.profile,
        "search_phase": value.candidate.action.phase.value,
        "impact": _impact_spec_document(value.candidate.action.impact),
        "source_topology": _topology_document(value.source_topology),
        "candidate_topology": _topology_document(value.candidate_topology),
        "target_sha256": value.candidate.target_sha256,
        "peer_sha256": value.candidate.peer_sha256,
        "trial_id": value.trial.trial_id,
        "expected_impact": _expected_impact_document(value.expected_impact),
        "objective": _objective_comparison_document(value.comparison),
        "valid": value.expected_impact.accepted and value.comparison.accepted,
        "reused_probe": value.reused_probe,
        "reused_visited": value.reused_visited,
        "rejection_reason": rejection_reason,
    }


def _artifact_document(value: object) -> dict[str, object]:
    return {
        "path": value.path,
        "schema": value.schema,
        "sha256": value.sha256,
        "size_bytes": value.size_bytes,
    }


def _pair_document(value: ConfigPair) -> dict[str, object]:
    return {
        "target": _artifact_document(value.target),
        "peer": _artifact_document(value.peer),
    }


def _object(value: object, location: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ControllerError(f"{location} must be an object")
    return value


def _exact_keys(
    value: dict[str, Any],
    expected: set[str],
    location: str,
) -> None:
    if set(value) != expected:
        raise ControllerError(f"{location} has invalid keys")


def _number(value: object, location: str) -> float:
    if type(value) not in (int, float):
        raise ControllerError(f"{location} must be a number")
    result = float(value)
    if not math.isfinite(result):
        raise ControllerError(f"{location} must be finite")
    return result


def _statistic_value(value: object, location: str) -> Statistic:
    document = _object(value, location)
    _exact_keys(
        document,
        {"samples", "median", "mad", "uncertainty", "unit"},
        location,
    )
    samples = document["samples"]
    if not isinstance(samples, list):
        raise ControllerError(f"{location}.samples must be an array")
    unit = document["unit"]
    if not isinstance(unit, str) or not unit:
        raise ControllerError(f"{location}.unit must be a non-empty string")
    return Statistic(
        samples=tuple(
            _number(item, f"{location}.samples[{index}]")
            for index, item in enumerate(samples)
        ),
        median=_number(document["median"], f"{location}.median"),
        mad=_number(document["mad"], f"{location}.mad"),
        uncertainty=_number(
            document["uncertainty"],
            f"{location}.uncertainty",
        ),
        unit=unit,
    )


def _objective_value(value: object, location: str) -> ObjectiveTrial:
    document = _object(value, location)
    _exact_keys(
        document,
        {
            "trial_id",
            "status",
            "client_p999",
            "server_throughput",
            "rejection_reason",
        },
        location,
    )
    trial_id = document["trial_id"]
    status = document["status"]
    if not isinstance(trial_id, str) or not isinstance(status, str):
        raise ControllerError(f"{location} has invalid identity")
    try:
        return ObjectiveTrial(
            trial_id=trial_id,
            status=status,
            client_p999=_statistic_value(
                document["client_p999"],
                f"{location}.client_p999",
            ),
            server_throughput=_statistic_value(
                document["server_throughput"],
                f"{location}.server_throughput",
            ),
            rejection_reason=document["rejection_reason"],
        )
    except (TypeError, ValueError) as error:
        raise ControllerError(f"{location} is invalid: {error}") from error


def _artifact_value(value: object, location: str) -> ArtifactRef:
    document = _object(value, location)
    _exact_keys(document, {"path", "schema", "sha256", "size_bytes"}, location)
    try:
        return ArtifactRef(
            path=document["path"],
            schema=document["schema"],
            sha256=document["sha256"],
            size_bytes=document["size_bytes"],
        )
    except (ContractError, TypeError) as error:
        raise ControllerError(f"{location} is invalid: {error}") from error


def _pair_value(value: object, location: str) -> ConfigPair:
    document = _object(value, location)
    _exact_keys(document, {"target", "peer"}, location)
    return ConfigPair(
        target=_artifact_value(document["target"], f"{location}.target"),
        peer=_artifact_value(document["peer"], f"{location}.peer"),
    )


@dataclasses.dataclass(frozen=True)
class _PersistedRoundBoundary:
    round_index: int
    previous_accepted: ConfigPair
    baseline_objective: ObjectiveTrial
    accepted_objective: ObjectiveTrial | None
    accepted_trial_id: str | None
    rollback_reason: str | None


def _round_boundary_document(
    *,
    round_index: int,
    previous_accepted: ConfigPair,
    baseline_objective: ObjectiveTrial,
    accepted_objective: ObjectiveTrial | None,
    accepted_trial_id: str | None,
    rollback_reason: str | None,
) -> dict[str, object]:
    return {
        "round_index": round_index,
        "previous_accepted": _pair_document(previous_accepted),
        "baseline_objective": _objective_document(baseline_objective),
        "accepted_objective": (
            _objective_document(accepted_objective)
            if accepted_objective is not None
            else None
        ),
        "accepted_trial_id": accepted_trial_id,
        "rollback_reason": rollback_reason,
    }


def _round_boundary_value(value: object) -> _PersistedRoundBoundary:
    location = "state.details.round_boundary"
    document = _object(value, location)
    _exact_keys(
        document,
        {
            "round_index",
            "previous_accepted",
            "baseline_objective",
            "accepted_objective",
            "accepted_trial_id",
            "rollback_reason",
        },
        location,
    )
    round_index = document["round_index"]
    if type(round_index) is not int or round_index < 1:
        raise ControllerError("persisted round index is invalid")
    accepted_trial_id = document["accepted_trial_id"]
    rollback_reason = document["rollback_reason"]
    if accepted_trial_id is not None and not isinstance(accepted_trial_id, str):
        raise ControllerError("persisted accepted trial ID is invalid")
    if rollback_reason is not None and rollback_reason not in (
        "no legal candidate",
        "no significant improvement",
        "all candidates invalid",
        "memory candidates exhausted without compute-bound evidence",
    ):
        raise ControllerError("persisted rollback reason is invalid")
    accepted_objective = (
        _objective_value(
            document["accepted_objective"],
            f"{location}.accepted_objective",
        )
        if document["accepted_objective"] is not None
        else None
    )
    if (accepted_objective is None) != (accepted_trial_id is None):
        raise ControllerError("persisted accepted objective identity is incomplete")
    if (
        accepted_objective is not None
        and accepted_objective.trial_id != accepted_trial_id
    ):
        raise ControllerError("persisted accepted objective identity is inconsistent")
    if (accepted_objective is None) == (rollback_reason is None):
        raise ControllerError("persisted round outcome is ambiguous")
    return _PersistedRoundBoundary(
        round_index=round_index,
        previous_accepted=_pair_value(
            document["previous_accepted"],
            f"{location}.previous_accepted",
        ),
        baseline_objective=_objective_value(
            document["baseline_objective"],
            f"{location}.baseline_objective",
        ),
        accepted_objective=accepted_objective,
        accepted_trial_id=accepted_trial_id,
        rollback_reason=rollback_reason,
    )


_STOP_REASONS = frozenset(
    (
        "no_legal_candidate",
        "no_significant_improvement",
        "all_candidates_invalid",
        "memory_candidates_exhausted_without_compute_evidence",
        "max_iterations",
        "infrastructure_failure_limit",
        "invalid_control_evidence",
    )
)


def inspect_convergence(
    store: TuningSessionStore,
    state: SessionState,
) -> ConvergenceSnapshot:
    """Validate and return the persisted convergence checkpoint."""

    document = _object(
        state.details.get("convergence"),
        "state.details.convergence",
    )
    _exact_keys(
        document,
        {
            "completed_rounds",
            "best",
            "best_trial_id",
            "best_objective",
            "stop_reason",
            "infrastructure_failures",
        },
        "state.details.convergence",
    )
    completed = document["completed_rounds"]
    failures = document["infrastructure_failures"]
    if type(completed) is not int or completed < 0:
        raise ControllerError("completed round count is invalid")
    if type(failures) is not int or failures < 0:
        raise ControllerError("infrastructure failure count is invalid")
    stop_reason = document["stop_reason"]
    if stop_reason is not None and stop_reason not in _STOP_REASONS:
        raise ControllerError("convergence stop reason is invalid")
    if state.phase != "complete" and stop_reason is not None:
        raise ControllerError("active convergence checkpoint has a stop reason")
    best_trial_id = document["best_trial_id"]
    if best_trial_id is not None and not isinstance(best_trial_id, str):
        raise ControllerError("historical best trial ID is invalid")
    best_objective = (
        _objective_value(
            document["best_objective"],
            "state.details.convergence.best_objective",
        )
        if document["best_objective"] is not None
        else None
    )
    if (best_objective is None) != (best_trial_id is None):
        raise ControllerError("historical best objective identity is incomplete")
    if best_objective is not None and best_objective.trial_id != best_trial_id:
        raise ControllerError("historical best objective identity is inconsistent")
    best = _pair_value(document["best"], "state.details.convergence.best")
    store.verify_artifact(best.target)
    store.verify_artifact(best.peer)
    return ConvergenceSnapshot(
        completed_rounds=completed,
        stop_reason=stop_reason,
        infrastructure_failures=failures,
        best=best,
        best_trial_id=best_trial_id,
        best_objective=best_objective,
    )


class TuningLoop:
    """Bound completed diagnosis rounds and persist the historical best."""

    def __init__(
        self,
        *,
        store: TuningSessionStore,
        round_runner: RoundRunner,
        policy: ObjectivePolicy,
    ) -> None:
        self._store = store
        self._round_runner = round_runner
        self._policy = policy

    @staticmethod
    def _stop_reason(state: SessionState) -> str:
        reason = state.details.get("reason")
        if reason == "no legal candidate":
            return "no_legal_candidate"
        if reason == "no significant improvement":
            return "no_significant_improvement"
        if reason == "all candidates invalid":
            return "all_candidates_invalid"
        if reason == "memory candidates exhausted without compute-bound evidence":
            return "memory_candidates_exhausted_without_compute_evidence"
        raise ControllerError("rolled-back round has no convergence stop reason")

    @staticmethod
    def _invalid_control_reason(state: SessionState) -> str | None:
        value = state.details.get("invalid_control_evidence")
        if value is None:
            return None
        document = _object(value, "state.details.invalid_control_evidence")
        _exact_keys(
            document,
            {"label", "trial_id", "reason"},
            "state.details.invalid_control_evidence",
        )
        if any(
            not isinstance(document[name], str) or not document[name]
            for name in ("label", "trial_id", "reason")
        ):
            raise ControllerError("invalid control evidence marker is invalid")
        return document["reason"]

    @staticmethod
    def _convergence_details(
        state: SessionState,
        *,
        completed_rounds: int,
        best: ConfigPair,
        best_objective: ObjectiveTrial | None,
        stop_reason: str | None,
        infrastructure_failures: int,
    ) -> dict[str, Any]:
        details = dict(state.details)
        details.pop("active_trial", None)
        details.pop("round_recovery", None)
        details.pop("baseline_health_failures", None)
        details.pop("probe_health_failures", None)
        details["convergence"] = {
            "completed_rounds": completed_rounds,
            "best": _pair_document(best),
            "best_trial_id": (
                best_objective.trial_id if best_objective is not None else None
            ),
            "best_objective": (
                _objective_document(best_objective)
                if best_objective is not None
                else None
            ),
            "stop_reason": stop_reason,
            "infrastructure_failures": infrastructure_failures,
        }
        return details

    def inspect(self, state: SessionState) -> ConvergenceSnapshot:
        return inspect_convergence(self._store, state)

    def _select_historical_best(
        self,
        *,
        best: ConfigPair,
        best_objective: ObjectiveTrial | None,
        previous_accepted: ConfigPair,
        baseline_objective: ObjectiveTrial,
        accepted: ConfigPair,
        accepted_objective: ObjectiveTrial | None,
    ) -> tuple[ConfigPair, ObjectiveTrial]:
        observations = [(previous_accepted, baseline_objective)]
        if accepted_objective is not None:
            observations.append((accepted, accepted_objective))
        if best_objective is not None:
            observations.insert(0, (best, best_objective))
        selected = select_historical_best(
            baseline=observations[0][1],
            accepted_trials=(item[1] for item in observations[1:]),
            policy=self._policy,
        )
        return next(
            item for item in observations if item[1].trial_id == selected.trial_id
        )

    def run(
        self,
        state: SessionState,
        *,
        max_iterations: int,
    ) -> ConvergenceResult:
        if type(max_iterations) is not int or max_iterations < 1:
            raise ValueError("max iterations must be a positive integer")
        if state.phase == "complete":
            snapshot = self.inspect(state)
            if snapshot.stop_reason is None:
                raise ControllerError("complete session has no stop reason")
            return ConvergenceResult(
                state=state,
                rounds=(),
                stop_reason=snapshot.stop_reason,
                completed_rounds=snapshot.completed_rounds,
                infrastructure_failures=snapshot.infrastructure_failures,
                best=snapshot.best,
                best_trial_id=snapshot.best_trial_id,
            )
        invalid_control_reason = self._invalid_control_reason(state)
        recovery_limit: InfrastructureFailureLimit | None = None
        active_attempt = any(
            attempt.status in ("pending", "running")
            for attempt in state.attempts
        )
        has_active_trial_cursor = state.details.get("active_trial") is not None
        if invalid_control_reason is None and (
            active_attempt or has_active_trial_cursor or state.phase not in (
            "baseline",
            "accepted",
            "rolled_back",
            "complete",
            )
        ):
            try:
                state = self._round_runner.recover_round(state).state
            except InfrastructureFailureLimit as error:
                state = error.state
                recovery_limit = error
            except InvalidControlEvidence as error:
                state = error.state
                invalid_control_reason = error.reason
        if (
            state.phase not in ("baseline", "accepted", "rolled_back")
            and recovery_limit is None
            and invalid_control_reason is None
        ):
            raise ControllerError(
                f"tuning loop requires a round boundary, got {state.phase}"
            )

        rounds: list[RoundResult] = []
        convergence = state.details.get("convergence")
        if convergence is None:
            completed_rounds = 0
            best = state.accepted
            best_objective = None
            infrastructure_failures = 0
        else:
            snapshot = self.inspect(state)
            completed_rounds = snapshot.completed_rounds
            best = snapshot.best
            best_objective = snapshot.best_objective
            infrastructure_failures = snapshot.infrastructure_failures
        boundary_value = state.details.get("round_boundary")
        recovery_value = state.details.get("round_recovery")
        recovery_round_index: int | None = None
        recovery_round_attempt = 1
        if recovery_value is not None:
            recovery_document = _object(
                recovery_value,
                "state.details.round_recovery",
            )
            _exact_keys(
                recovery_document,
                {"round_index", "next_attempt"},
                "state.details.round_recovery",
            )
            recovery_round_index = recovery_document["round_index"]
            recovery_round_attempt = recovery_document["next_attempt"]
            if (
                type(recovery_round_index) is not int
                or recovery_round_index < 1
                or type(recovery_round_attempt) is not int
                or recovery_round_attempt < 2
            ):
                raise ControllerError("round recovery cursor is invalid")
            if recovery_round_index != completed_rounds + 1:
                raise ControllerError("round recovery cursor does not follow history")
        if boundary_value is not None:
            boundary = _round_boundary_value(boundary_value)
            if boundary.round_index < completed_rounds:
                raise ControllerError("persisted round boundary moved backwards")
            if boundary.round_index > completed_rounds:
                if boundary.round_index != completed_rounds + 1:
                    raise ControllerError("persisted round boundary has a gap")
                best, best_objective = self._select_historical_best(
                    best=best,
                    best_objective=best_objective,
                    previous_accepted=boundary.previous_accepted,
                    baseline_objective=boundary.baseline_objective,
                    accepted=state.accepted,
                    accepted_objective=boundary.accepted_objective,
                )
                completed_rounds = boundary.round_index
                state = self._store.checkpoint(
                    state,
                    details=self._convergence_details(
                        state,
                        completed_rounds=completed_rounds,
                        best=best,
                        best_objective=best_objective,
                        stop_reason=None,
                        infrastructure_failures=0,
                    ),
                )
        elif (
            state.phase != "baseline"
            and recovery_round_index is None
            and recovery_limit is None
            and invalid_control_reason is None
        ):
            raise ControllerError("round boundary has no recoverable outcome")
        if recovery_limit is not None:
            stop_reason = "infrastructure_failure_limit"
            state = self._store.transition(
                state,
                phase="complete",
                details=self._convergence_details(
                    state,
                    completed_rounds=completed_rounds,
                    best=best,
                    best_objective=best_objective,
                    stop_reason=stop_reason,
                    infrastructure_failures=recovery_limit.consecutive_failures,
                ),
            )
            return ConvergenceResult(
                state=state,
                rounds=(),
                stop_reason=stop_reason,
                completed_rounds=completed_rounds,
                infrastructure_failures=recovery_limit.consecutive_failures,
                best=best,
                best_trial_id=(
                    best_objective.trial_id if best_objective is not None else None
                ),
            )
        if invalid_control_reason is not None:
            stop_reason = "invalid_control_evidence"
            state = self._store.transition(
                state,
                phase="complete",
                details=self._convergence_details(
                    state,
                    completed_rounds=completed_rounds,
                    best=best,
                    best_objective=best_objective,
                    stop_reason=stop_reason,
                    infrastructure_failures=infrastructure_failures,
                ),
            )
            return ConvergenceResult(
                state=state,
                rounds=(),
                stop_reason=stop_reason,
                completed_rounds=completed_rounds,
                infrastructure_failures=infrastructure_failures,
                best=best,
                best_trial_id=(
                    best_objective.trial_id if best_objective is not None else None
                ),
            )
        if state.phase == "rolled_back" and recovery_round_index is None:
            stop_reason = self._stop_reason(state)
            state = self._store.transition(
                state,
                phase="complete",
                details=self._convergence_details(
                    state,
                    completed_rounds=completed_rounds,
                    best=best,
                    best_objective=best_objective,
                    stop_reason=stop_reason,
                    infrastructure_failures=infrastructure_failures,
                ),
            )
            return ConvergenceResult(
                state=state,
                rounds=(),
                stop_reason=stop_reason,
                completed_rounds=completed_rounds,
                infrastructure_failures=infrastructure_failures,
                best=best,
                best_trial_id=(
                    best_objective.trial_id if best_objective is not None else None
                ),
            )
        stop_reason = "max_iterations"
        # Schema v1 intentionally has no throughput-target completion condition.
        for round_index in range(completed_rounds + 1, max_iterations + 1):
            round_attempt = (
                recovery_round_attempt
                if round_index == recovery_round_index
                else 1
            )
            try:
                result = self._round_runner.run_round(
                    state,
                    round_index=round_index,
                    round_attempt=round_attempt,
                )
            except InfrastructureFailureLimit as error:
                state = error.state
                infrastructure_failures = error.consecutive_failures
                stop_reason = "infrastructure_failure_limit"
                break
            except InvalidControlEvidence as error:
                state = error.state
                stop_reason = "invalid_control_evidence"
                break
            rounds.append(result)
            completed_rounds += 1
            state = result.state
            best, best_objective = self._select_historical_best(
                best=best,
                best_objective=best_objective,
                previous_accepted=result.previous_accepted,
                baseline_objective=result.baseline_objective,
                accepted=state.accepted,
                accepted_objective=result.accepted_objective,
            )
            state = self._store.checkpoint(
                state,
                details=self._convergence_details(
                    state,
                    completed_rounds=completed_rounds,
                    best=best,
                    best_objective=best_objective,
                    stop_reason=None,
                    infrastructure_failures=0,
                ),
            )
            if result.accepted_trial_id is None:
                stop_reason = self._stop_reason(state)
                break

        details = self._convergence_details(
            state,
            completed_rounds=completed_rounds,
            best=best,
            best_objective=best_objective,
            stop_reason=stop_reason,
            infrastructure_failures=infrastructure_failures,
        )
        state = self._store.transition(
            state,
            phase="complete",
            details=details,
        )
        return ConvergenceResult(
            state=state,
            rounds=tuple(rounds),
            stop_reason=stop_reason,
            completed_rounds=completed_rounds,
            infrastructure_failures=infrastructure_failures,
            best=best,
            best_trial_id=(
                best_objective.trial_id if best_objective is not None else None
            ),
        )


__all__ = [
    "ColdStartController",
    "ConvergenceResult",
    "ConvergenceSnapshot",
    "ControllerError",
    "InfrastructureFailureLimit",
    "InvalidControlEvidence",
    "inspect_convergence",
    "MeasureTrialExecutor",
    "RoundResult",
    "RoundRecovery",
    "TrialExecutionError",
    "TrialExecutor",
    "TuningLoop",
]
