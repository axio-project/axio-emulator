"""Paper-aligned artifact-evaluation case matrices."""

from __future__ import annotations

from artifact_eval.configuration import CaseConfiguration
from artifact_eval.harness import ExperimentCase
from artifact_eval.model import RunProfile


E2E_HANDLERS = ("t_app", "l_app", "m_app")
E2E_REQUESTS = (
    (128, 86),
    (512, 470),
    (1024, 982),
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
                case_id=(
                    f"{backend}-{handler.replace('_', '-')}-req{frame_bytes}"
                ),
                backend=backend,
                handler=handler,
                c1=16,
                c2=8 if backend == "roce" else 16,
                c3=64 if backend == "roce" else 32,
                warmup_windows=warmup,
                sample_windows=sample,
                request_frame_bytes=frame_bytes,
                request_payload_bytes=request_payload,
                response_payload_bytes=(
                    22 if handler == "t_app" else request_payload
                ),
            ),
            mode="bootstrap",
            sessions=sessions,
            tuning_rounds=rounds,
        )
        for backend in ("dpdk", "roce")
        for handler in E2E_HANDLERS
        for frame_bytes, request_payload in E2E_REQUESTS
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
        )
        for handler in ("l_app", "m_app")
        for c1 in (1, 2, 4, 8, 16)
    )


def figure7_cases(
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
                case_id=f"{handler.replace('_', '-')}-c1c2-{count:03d}",
                backend="dpdk",
                handler=handler,
                c1=count,
                c2=count,
                c3=128,
                warmup_windows=warmup,
                sample_windows=sample,
                stage_distribution=True,
            ),
            mode="measure",
            repeats=repeat_count,
        )
        for handler in ("l_app", "t_app")
        for count in (1, 2, 4, 8, 16)
    )


def figure8_cases(
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
        *(("t_app", 4, 4, value) for value in (32, 64, 128, 256, 512)),
        *(("l_app", 8, 1, value) for value in (16, 32, 64, 128, 256)),
    )
    return tuple(
        ExperimentCase(
            configuration=CaseConfiguration(
                case_id=f"{handler.replace('_', '-')}-c3-{c3:03d}",
                backend="dpdk",
                handler=handler,
                c1=c1,
                c2=c2,
                c3=c3,
                warmup_windows=warmup,
                sample_windows=sample,
                stage_distribution=True,
            ),
            mode="measure",
            repeats=repeat_count,
        )
        for handler, c1, c2, c3 in points
    )


def figure14_cases(
    profile: RunProfile,
    *,
    warmup_windows: int | None = None,
    sample_windows: int | None = None,
    tuning_rounds: int | None = None,
) -> tuple[ExperimentCase, ...]:
    warmup = profile.warmup_windows if warmup_windows is None else warmup_windows
    sample = profile.sample_windows if sample_windows is None else sample_windows
    rounds = profile.tuning_rounds if tuning_rounds is None else tuning_rounds
    return (
        ExperimentCase(
            configuration=CaseConfiguration(
                case_id="dpdk-packet-echo",
                backend="dpdk",
                handler="t_app",
                packet_handler="echo",
                c1=16,
                c2=16,
                c3=32,
                warmup_windows=warmup,
                sample_windows=sample,
            ),
            mode="bootstrap",
            tuning_rounds=rounds,
        ),
        ExperimentCase(
            configuration=CaseConfiguration(
                case_id="roce-file-write",
                backend="roce",
                handler="file_write",
                c1=16,
                c2=8,
                c3=16,
                warmup_windows=warmup,
                sample_windows=sample,
            ),
            mode="bootstrap",
            tuning_rounds=rounds,
        ),
    )
