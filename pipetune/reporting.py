"""Deterministic user-facing outputs for a persisted PipeTune session."""

from __future__ import annotations

import dataclasses
import hashlib
import json
import os
import pathlib
import tempfile
from typing import Any

from pipetune.controller import ConvergenceResult, inspect_convergence
from pipetune.model import ArtifactRef, COUNTER_NAMES
from pipetune.session import SessionState, TuningSessionStore


class ReportingError(RuntimeError):
    """Raised when persisted tuning evidence cannot form a report."""


@dataclasses.dataclass(frozen=True)
class SessionPublication:
    best: pathlib.Path
    peer: pathlib.Path
    iterations: pathlib.Path
    report: pathlib.Path
    stop_reason: str


def _json_bytes(document: object, *, compact: bool = False) -> bytes:
    options: dict[str, object] = {
        "allow_nan": False,
        "ensure_ascii": False,
        "sort_keys": True,
    }
    if compact:
        options["separators"] = (",", ":")
    else:
        options["indent"] = 2
    return (json.dumps(document, **options) + "\n").encode("utf-8")


def _write_atomic(path: pathlib.Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file() and path.read_bytes() == payload:
        return
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = pathlib.Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        if hasattr(os, "O_DIRECTORY"):
            directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def _copy_verified(
    store: TuningSessionStore,
    reference: ArtifactRef,
    destination: pathlib.Path,
) -> None:
    source = store.verify_artifact(reference)
    payload = source.read_bytes()
    if (
        len(payload) != reference.size_bytes
        or hashlib.sha256(payload).hexdigest() != reference.sha256
    ):
        raise ReportingError(f"verified artifact changed during copy: {reference.path}")
    _write_atomic(destination, payload)


def _round_outcomes(history: tuple[SessionState, ...]) -> tuple[SessionState, ...]:
    by_round: dict[int, SessionState] = {}
    for state in history:
        boundary = state.details.get("round_boundary")
        round_index = state.details.get("round")
        if (
            state.phase in ("accepted", "rolled_back")
            and isinstance(boundary, dict)
            and type(round_index) is int
            and round_index > 0
        ):
            by_round[round_index] = state
    return tuple(by_round[index] for index in sorted(by_round))


def _diagnosis_for_round(
    history: tuple[SessionState, ...],
    outcome: SessionState,
) -> dict[str, object] | None:
    direct = outcome.details.get("diagnosis_document")
    if isinstance(direct, dict):
        return direct
    round_index = outcome.details.get("round")
    for state in reversed(history[: outcome.generation + 1]):
        candidate = state.details.get("diagnosis_document")
        if state.details.get("round") == round_index and isinstance(candidate, dict):
            return candidate
    return None


def _iteration_record(
    history: tuple[SessionState, ...],
    state: SessionState,
) -> dict[str, object]:
    boundary = state.details["round_boundary"]
    if not isinstance(boundary, dict):
        raise ReportingError("round boundary is not an object")
    round_index = state.details["round"]
    evaluations = state.details.get("candidate_evaluations", [])
    if not isinstance(evaluations, list):
        raise ReportingError("candidate evaluations are not an array")
    diagnosis = _diagnosis_for_round(history, state)
    baseline_trial_id = state.details.get("baseline_trial_id")
    if not isinstance(baseline_trial_id, str):
        baseline_objective = boundary.get("baseline_objective")
        if isinstance(baseline_objective, dict):
            baseline_trial_id = baseline_objective.get("trial_id")
    if not isinstance(baseline_trial_id, str):
        raise ReportingError(f"round {round_index} has no baseline trial")
    accepted_trial = boundary.get("accepted_trial_id")
    rollback_reason = boundary.get("rollback_reason")
    probe_id = None
    if isinstance(diagnosis, dict):
        trial = diagnosis.get("trial")
        if isinstance(trial, dict) and isinstance(trial.get("probe"), str):
            probe_id = trial["probe"]
    return {
        "baseline": {
            "artifact": f"trials/{baseline_trial_id}/trial.json",
            "objective": boundary.get("baseline_objective"),
            "trial_id": baseline_trial_id,
        },
        "candidates": [
            {
                **evaluation,
                "artifact": f"trials/{evaluation.get('trial_id')}/trial.json",
            }
            for evaluation in evaluations
            if isinstance(evaluation, dict)
        ],
        "diagnosis": diagnosis,
        "diagnosis_state": state.state_path,
        "probe": (
            {
                "artifact": f"trials/{probe_id}/trial.json",
                "trial_id": probe_id,
            }
            if probe_id is not None
            else None
        ),
        "outcome": {
            "accepted_trial_id": accepted_trial,
            "kind": "accepted" if accepted_trial is not None else "rolled_back",
            "rollback_reason": rollback_reason,
        },
        "paired_search": state.details.get("paired_search"),
        "round": round_index,
        "schema": "pipetune.iteration/v1",
    }


def _terminal_partial_record(
    history: tuple[SessionState, ...],
    completed_rounds: set[int],
) -> dict[str, object] | None:
    final = history[-1]
    round_index = final.details.get("round")
    if type(round_index) is not int or round_index < 1 or round_index in completed_rounds:
        return None
    evidence = next(
        (
            state
            for state in reversed(history)
            if state.details.get("round") == round_index
            and isinstance(state.details.get("baseline_trial_id"), str)
        ),
        None,
    )
    baseline_id = (
        evidence.details["baseline_trial_id"] if evidence is not None else None
    )
    invalid_control = final.details.get("invalid_control_evidence")
    if baseline_id is None and isinstance(invalid_control, dict):
        marker_trial = invalid_control.get("trial_id")
        if isinstance(marker_trial, str):
            baseline_id = marker_trial
    evaluations = final.details.get("candidate_evaluations", [])
    if not isinstance(evaluations, list):
        evaluations = []
    diagnosis = (
        _diagnosis_for_round(history, evidence)
        if evidence is not None
        else None
    )
    probe_id = None
    if isinstance(diagnosis, dict):
        trial = diagnosis.get("trial")
        if isinstance(trial, dict):
            probe_id = trial.get("probe")
    convergence = final.details.get("convergence")
    stop_reason = (
        convergence.get("stop_reason") if isinstance(convergence, dict) else None
    )
    previous_attempt_count = 0
    for state in reversed(history[:-1]):
        if isinstance(state.details.get("round_boundary"), dict):
            previous_attempt_count = len(state.attempts)
            break
    return {
        "baseline": (
            {
                "artifact": f"trials/{baseline_id}/trial.json",
                "objective": None,
                "trial_id": baseline_id,
            }
            if isinstance(baseline_id, str)
            else None
        ),
        "candidates": [
            {
                **evaluation,
                "artifact": f"trials/{evaluation.get('trial_id')}/trial.json",
            }
            for evaluation in evaluations
            if isinstance(evaluation, dict)
        ],
        "diagnosis": diagnosis,
        "diagnosis_state": (
            evidence.state_path if evidence is not None else final.state_path
        ),
        "outcome": {
            "accepted_trial_id": None,
            "kind": "terminated",
            "rollback_reason": stop_reason,
        },
        "paired_search": final.details.get("paired_search"),
        "probe": (
            {
                "artifact": f"trials/{probe_id}/trial.json",
                "trial_id": probe_id,
            }
            if isinstance(probe_id, str)
            else None
        ),
        "round": round_index,
        "schema": "pipetune.iteration/v1",
        "attempts": [
            {
                "artifact": (
                    attempt.manifest.path if attempt.manifest is not None else None
                ),
                "status": attempt.status,
                "trial_id": attempt.trial_id,
            }
            for attempt in final.attempts[previous_attempt_count:]
        ],
    }


def _format_number(value: object) -> str:
    if type(value) not in (int, float):
        return "unavailable"
    return f"{float(value):.2f}"


def _format_statistic(value: object) -> str:
    if not isinstance(value, dict):
        return "unavailable"
    return (
        f"{_format_number(value.get('median'))} ± "
        f"{_format_number(value.get('uncertainty'))}"
    )


def _comparison_summary(
    value: object,
    *,
    observed_field: str,
    required_field: str,
) -> str:
    if not isinstance(value, dict):
        return "unavailable"
    status = "pass" if value.get("accepted") else "reject"
    metric = value.get("metric")
    if not metric:
        reason = value.get("reason")
        return f"{status}: {reason}" if reason else f"{status}: unavailable"
    observed = _format_number(value.get(observed_field))
    required = _format_number(value.get(required_field))
    return f"{status}: {metric}, observed {observed}, required {required}"


def _topology_summary(value: object) -> str:
    if not isinstance(value, dict):
        return "unavailable"
    names = (
        ("A", "application_count"),
        ("D", "dispatcher_count"),
        ("O", "overlap_count"),
        ("P", "physical_core_count"),
    )
    if any(type(value.get(field)) is not int for _, field in names):
        return "unavailable"
    return "/".join(f"{label}{value[field]}" for label, field in names)


def _topology_delta(candidate: dict[str, object]) -> str:
    return (
        f"{_topology_summary(candidate.get('source_topology'))} → "
        f"{_topology_summary(candidate.get('candidate_topology'))}"
    )


def _report_markdown(
    records: tuple[dict[str, object], ...],
    result: ConvergenceResult,
) -> str:
    lines = [
        "# PipeTune tuning report",
        "",
        f"- Stop reason: `{result.stop_reason}`",
        f"- Completed diagnosis rounds: {result.completed_rounds}",
        f"- Historical best trial: `{result.best_trial_id or 'initial configuration'}`",
        "- Published pair: [best.toml](best.toml) + [peer.toml](peer.toml)",
        "",
        "## Search policy",
        "",
        "The target's NUMA workspace budget `U` bounds physical-core use. "
        "In the memory phase, a colocated one-to-one target first reduces "
        "C1/C2 together. If a "
        "one-core reduction does not improve E2E performance while both "
        "directional miss rates exceed 40%, PipeTune uses binary count probes "
        "to locate the largest count where both rates are at most 40%.",
        "",
        "In the compute phase, application expansion compares one-to-one split "
        "and balanced fanout "
        "placements; dispatcher expansion remains one-to-one. The memory-search "
        "cursor may cross a temporary E2E regression, but only an objective "
        "winner can replace the historical best. An enqueue drop rejects only "
        "the candidate that produced it; it does not diagnose a contention "
        "point or move the search cursor. Rejected probes and exploratory "
        "cursors never replace `best.toml`.",
        "",
        "## Search cursor trajectory",
        "",
        "| Iteration | Phase | Action | Topology | Acceptance | Trial |",
        "| ---: | --- | --- | --- | --- | --- |",
    ]
    accepted_rows = 0
    for record in records:
        outcome = record.get("outcome")
        candidates = record.get("candidates")
        if not isinstance(outcome, dict) or not isinstance(candidates, list):
            continue
        accepted_id = outcome.get("accepted_trial_id")
        accepted = next(
            (
                candidate
                for candidate in candidates
                if isinstance(candidate, dict)
                and candidate.get("trial_id") == accepted_id
            ),
            None,
        )
        if accepted is None:
            continue
        objective = accepted.get("objective")
        acceptance_mode = (
            objective.get("acceptance_mode")
            if isinstance(objective, dict)
            else None
        )
        if (
            not acceptance_mode
            and str(accepted.get("action", "")).startswith("paired-colocated-")
        ):
            acceptance_mode = "exploratory_memory_cursor"
        lines.append(
            "| {round} | `{phase}` | `{action}` | {topology} | `{mode}` | "
            "[{trial}]({artifact}) |".format(
                round=record.get("round"),
                phase=accepted.get("search_phase", "unavailable"),
                action=accepted.get("action", accepted.get("candidate_id")),
                topology=_topology_delta(accepted),
                mode=acceptance_mode or "unavailable",
                trial=accepted.get("trial_id"),
                artifact=accepted.get("artifact"),
            )
        )
        accepted_rows += 1
    if accepted_rows == 0:
        lines.append("| none | n/a | n/a | n/a | n/a | n/a |")
    lines.append("")
    for record in records:
        round_index = record["round"]
        baseline = record["baseline"]
        diagnosis = record.get("diagnosis")
        baseline_line = "- Baseline: unavailable (trial did not publish)"
        if isinstance(baseline, dict):
            baseline_line = (
                f"- Baseline: [{baseline['trial_id']}]({baseline['artifact']})"
            )
        lines.extend(
            [
                f"## Iteration {round_index}",
                "",
                baseline_line,
            ]
        )
        paired_search = record.get("paired_search")
        if isinstance(paired_search, dict):
            mode = paired_search.get("mode", "unavailable")
            if isinstance(mode, str) and mode.startswith("binary_"):
                lines.append(
                    "- Binary paired search: mode `{mode}`, threshold {threshold}%, "
                    "pressure bound {high}, relief bound {low}, next A/D count: "
                    "{next_count}".format(
                        mode=mode,
                        threshold=_format_number(paired_search.get("threshold")),
                        high=paired_search.get("high_pressure_count"),
                        low=paired_search.get("low_relief_count"),
                        next_count=paired_search.get("next_count"),
                    )
                )
        probe = record.get("probe")
        if isinstance(probe, dict):
            lines.append(
                f"- Required C1 perturbation: [{probe['trial_id']}]({probe['artifact']})"
            )
        diagnosis_state = record.get("diagnosis_state")
        if isinstance(diagnosis_state, str):
            lines.append(
                f"- Diagnosis evidence: [persisted state]({diagnosis_state})"
            )
        result_document: dict[str, object] = {}
        counter_rates: dict[str, object] = {}
        probe_counter_rates: dict[str, object] = {}
        if isinstance(diagnosis, dict):
            value = diagnosis.get("result")
            if isinstance(value, dict):
                result_document = value
            rates = diagnosis.get("counter_rates")
            if isinstance(rates, dict) and isinstance(rates.get("baseline"), dict):
                counter_rates = rates["baseline"]
                if isinstance(rates.get("probe"), dict):
                    probe_counter_rates = rates["probe"]
        lines.extend(
            [
                "- Diagnosis is a hypothesis: "
                f"`{result_document.get('point', 'unavailable')}` "
                f"({result_document.get('direction', 'n/a')}, "
                f"confidence {result_document.get('confidence', 'unavailable')})",
                "",
                "### Four-rate evidence",
                "",
                "| Rate | Baseline median ± uncertainty (%) | Probe median ± uncertainty (%) |",
                "| --- | ---: | ---: |",
            ]
        )
        for name in COUNTER_NAMES:
            baseline_rate = _format_statistic(counter_rates.get(name))
            probe_rate = _format_statistic(probe_counter_rates.get(name))
            lines.append(f"| `{name}` | {baseline_rate} | {probe_rate} |")
        lines.extend(
            [
                "",
                "### Probes and rejected candidates",
                "",
                "| Phase | Candidate | Topology | Expected impact | End-to-end objective | Decision | Trial |",
                "| --- | --- | --- | --- | --- | --- | --- |",
            ]
        )
        candidates = record["candidates"]
        outcome = record["outcome"]
        accepted_id = (
            outcome.get("accepted_trial_id")
            if isinstance(outcome, dict)
            else None
        )
        remaining = (
            [
                candidate
                for candidate in candidates
                if isinstance(candidate, dict)
                and candidate.get("trial_id") != accepted_id
            ]
            if isinstance(candidates, list)
            else []
        )
        if remaining:
            for candidate in remaining:
                expected = candidate.get("expected_impact", {})
                objective = candidate.get("objective", {})
                expected_status = _comparison_summary(
                    expected,
                    observed_field="observed_reduction",
                    required_field="required_reduction",
                )
                objective_status = _comparison_summary(
                    objective,
                    observed_field="observed_improvement",
                    required_field="required_improvement",
                )
                decision = "not selected" if candidate.get("valid") else "rollback"
                lines.append(
                    "| `{phase}` | `{action}` | {topology} | {expected} | "
                    "{objective} | {decision} | "
                    "[{trial}]({artifact}) |".format(
                        phase=candidate.get("search_phase", "unavailable"),
                        action=candidate.get("action", candidate.get("candidate_id")),
                        topology=_topology_delta(candidate),
                        expected=expected_status,
                        objective=objective_status,
                        decision=decision,
                        trial=candidate.get("trial_id"),
                        artifact=candidate.get("artifact"),
                    )
                )
        else:
            lines.append("| none | n/a | n/a | n/a | n/a | n/a | n/a |")
        attempts = record.get("attempts")
        if isinstance(attempts, list) and attempts:
            lines.extend(["", "### Terminal attempts", ""])
            for attempt in attempts:
                artifact = attempt.get("artifact")
                if isinstance(artifact, str):
                    lines.append(
                        f"- `{attempt.get('trial_id')}`: "
                        f"[{attempt.get('status')}]({artifact})"
                    )
                else:
                    lines.append(
                        f"- `{attempt.get('trial_id')}`: {attempt.get('status')} "
                        "(no manifest published)"
                    )
        lines.extend(
            [
                "",
                f"Outcome: `{outcome['kind']}`"
                + (
                    f" (`{outcome['rollback_reason']}`)"
                    if outcome.get("rollback_reason")
                    else ""
                ),
                "",
            ]
        )
    lines.extend(
        [
            "## Residual manual knobs",
            "",
            "The script intentionally leaves these build-sensitive choices for a "
            "separate, manually reviewed run:",
            "",
            "- C4: inflight depth",
            "- C5: MTU",
            "- C6: memory-pool handler",
            "",
        ]
    )
    return "\n".join(lines)


def publish_session_outputs(
    root: pathlib.Path,
    store: TuningSessionStore,
    result: ConvergenceResult,
) -> SessionPublication:
    """Publish mutable summaries derived only from verified immutable state."""

    root = root.resolve()
    history = store.history()
    if history[-1] != result.state:
        raise ReportingError("convergence result is not the current session state")
    snapshot = inspect_convergence(store, history[-1])
    if (
        snapshot.stop_reason != result.stop_reason
        or snapshot.completed_rounds != result.completed_rounds
        or snapshot.infrastructure_failures != result.infrastructure_failures
        or snapshot.best != result.best
        or snapshot.best_trial_id != result.best_trial_id
    ):
        raise ReportingError("convergence result does not match persisted state")
    outcomes = _round_outcomes(history)
    records_list = [
        _iteration_record(history, state) for state in outcomes
    ]
    partial = _terminal_partial_record(
        history,
        {int(record["round"]) for record in records_list},
    )
    if partial is not None:
        records_list.append(partial)
    records = tuple(records_list)
    best_path = root / "best.toml"
    peer_path = root / "peer.toml"
    iterations_path = root / "iterations.jsonl"
    report_path = root / "report.md"
    _copy_verified(store, result.best.target, best_path)
    _copy_verified(store, result.best.peer, peer_path)
    _write_atomic(
        iterations_path,
        b"".join(_json_bytes(record, compact=True) for record in records),
    )
    _write_atomic(
        report_path,
        _report_markdown(records, result).encode("utf-8"),
    )
    return SessionPublication(
        best=best_path,
        peer=peer_path,
        iterations=iterations_path,
        report=report_path,
        stop_reason=result.stop_reason,
    )


def _published_artifact(root: pathlib.Path, name: str) -> dict[str, object] | None:
    path = root / name
    if not path.is_file():
        return None
    payload = path.read_bytes()
    return {
        "path": name,
        "sha256": hashlib.sha256(payload).hexdigest(),
        "size_bytes": len(payload),
    }


def status_document(
    root: pathlib.Path,
    store: TuningSessionStore,
) -> dict[str, object]:
    """Render read-only session status; never repair or republish files."""

    root = root.resolve()
    state = store.status()
    convergence = state.details.get("convergence")
    if convergence is None:
        if state.phase == "complete":
            raise ReportingError("complete session has no convergence checkpoint")
        completed_rounds = 0
        stop_reason = None
    else:
        snapshot = inspect_convergence(store, state)
        completed_rounds = snapshot.completed_rounds
        stop_reason = snapshot.stop_reason
    return {
        "completed_rounds": completed_rounds,
        "generation": state.generation,
        "outputs": {
            name: _published_artifact(root, name)
            for name in ("best.toml", "peer.toml", "iterations.jsonl", "report.md")
        },
        "phase": state.phase,
        "schema": "pipetune.status/v1",
        "session_id": state.session_id,
        "stop_reason": stop_reason,
    }


__all__ = [
    "ReportingError",
    "SessionPublication",
    "publish_session_outputs",
    "status_document",
]
