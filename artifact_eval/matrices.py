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
