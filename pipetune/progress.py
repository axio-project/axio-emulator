"""Optional human progress and completion summaries for PipeTune front ends."""

from __future__ import annotations

import dataclasses
import pathlib
from collections.abc import Callable


ProgressSink = Callable[[str], None]


def emit(progress: ProgressSink | None, message: str) -> None:
    """Publish one complete progress line when a caller requested progress."""

    if progress is not None:
        progress(message)


@dataclasses.dataclass(frozen=True)
class TuningSummary:
    """The historical-best values needed by the interactive CLI."""

    target_throughput_mpps: float
    client_p999_us: float
    application_core_count: int
    dispatcher_queue_count: int
    app_rx_batch_size: int
    app_tx_batch_size: int
    dispatcher_rx_batch_size: int
    dispatcher_tx_batch_size: int
    nic_rx_post_size: int
    nic_tx_post_size: int
    report_path: pathlib.Path


def format_tuning_summary(
    summary: TuningSummary,
    *,
    completed_rounds: int,
    stop_reason: str | None,
) -> str:
    """Render a compact terminal summary without changing stdout JSON."""

    return "\n".join(
        (
            "PipeTune complete",
            f"  Best target throughput: {summary.target_throughput_mpps:.2f} Mpps",
            f"  Client P99.9: {summary.client_p999_us:.2f} us",
            (
                "  Best C1/C2: "
                f"{summary.application_core_count}/"
                f"{summary.dispatcher_queue_count}"
            ),
            (
                "  Best C3: app RX/TX "
                f"{summary.app_rx_batch_size}/{summary.app_tx_batch_size}, "
                "dispatcher RX/TX "
                f"{summary.dispatcher_rx_batch_size}/"
                f"{summary.dispatcher_tx_batch_size}, NIC RX/TX "
                f"{summary.nic_rx_post_size}/{summary.nic_tx_post_size}"
            ),
            f"  Completed rounds: {completed_rounds}",
            f"  Stop reason: {stop_reason or 'in progress'}",
            f"  Report: {summary.report_path}",
        )
    )


__all__ = [
    "emit",
    "format_tuning_summary",
    "ProgressSink",
    "TuningSummary",
]
