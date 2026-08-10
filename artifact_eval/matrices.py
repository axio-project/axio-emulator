"""Paper-aligned artifact-evaluation case matrices."""

from __future__ import annotations

from artifact_eval.configuration import CaseConfiguration
from artifact_eval.harness import ExperimentCase
from artifact_eval.model import RunProfile


HANDLERS = (
    "t_app",
    "l_app",
    "m_app",
    "file_write",
    "file_read",
    "key_value",
)


def end_to_end_cases(
    profile: RunProfile,
    *,
    sessions: int = 1,
    warmup_windows: int | None = None,
    sample_windows: int | None = None,
    tuning_rounds: int | None = None,
) -> tuple[ExperimentCase, ...]:
    warmup = profile.warmup_windows if warmup_windows is None else warmup_windows
    sample = profile.sample_windows if sample_windows is None else sample_windows
    rounds = profile.tuning_rounds if tuning_rounds is None else tuning_rounds
    return tuple(
        ExperimentCase(
            configuration=CaseConfiguration(
                case_id=f"{backend}-{handler.replace('_', '-')}",
                backend=backend,
                handler=handler,
                c1=16,
                c2=16,
                c3=32,
                warmup_windows=warmup,
                sample_windows=sample,
            ),
            mode="bootstrap",
            sessions=sessions,
            tuning_rounds=rounds,
        )
        for backend in ("dpdk", "roce")
        for handler in HANDLERS
    )


def figure3_cases(
    profile: RunProfile,
    *,
    repeats: int | None = None,
    warmup_windows: int | None = None,
    sample_windows: int | None = None,
) -> tuple[ExperimentCase, ...]:
    warmup = profile.warmup_windows if warmup_windows is None else warmup_windows
    sample = profile.sample_windows if sample_windows is None else sample_windows
    repeat_count = profile.repeats if repeats is None else repeats
    points = (
        *((f"c1-{value:03d}", value, 4, 32) for value in (4, 8, 12, 16)),
        *((f"c2-{value:03d}", 16, value, 32) for value in (4, 8, 12, 16)),
        *((f"c3-{value:03d}", 8, 4, value) for value in (16, 32, 64, 128)),
    )
    return tuple(
        ExperimentCase(
            configuration=CaseConfiguration(
                case_id=case_id,
                backend="dpdk",
                handler="l_app",
                c1=c1,
                c2=c2,
                c3=c3,
                warmup_windows=warmup,
                sample_windows=sample,
            ),
            mode="measure",
            repeats=repeat_count,
        )
        for case_id, c1, c2, c3 in points
    )


def figure6_cases(
    profile: RunProfile,
    *,
    repeats: int | None = None,
    warmup_windows: int | None = None,
    sample_windows: int | None = None,
) -> tuple[ExperimentCase, ...]:
    warmup = profile.warmup_windows if warmup_windows is None else warmup_windows
    sample = profile.sample_windows if sample_windows is None else sample_windows
    repeat_count = profile.repeats if repeats is None else repeats
    return tuple(
        ExperimentCase(
            configuration=CaseConfiguration(
                case_id=f"{handler.replace('_', '-')}-c1-{c1:03d}",
                backend="dpdk",
                handler=handler,
                c1=c1,
                c2=1,
                c3=16,
                warmup_windows=warmup,
                sample_windows=sample,
                stage_distribution=True,
            ),
            mode="measure",
            repeats=repeat_count,
            target_role="client" if handler == "l_app" else "server",
        )
        for handler in ("l_app", "m_app")
        for c1 in (1, 2, 4, 8, 16)
    )
