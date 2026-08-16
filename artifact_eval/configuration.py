"""Materialize experiment configurations through axio-configure."""

from __future__ import annotations

import dataclasses
import json
import pathlib
import subprocess


class ConfigurationError(RuntimeError):
    pass


_PAYLOADS = {
    "t_app": (86, 22),
    "l_app": (86, 86),
    "m_app": (86, 86),
    "file_write": (16384, 22),
    "file_read": (22, 102400),
    "key_value": (81, 81),
}
_LIFECYCLE_SLACK_WINDOWS = 5


def _trial_iterations(case: CaseConfiguration) -> int:
    return case.warmup_windows + case.sample_windows + _LIFECYCLE_SLACK_WINDOWS


@dataclasses.dataclass(frozen=True)
class CaseConfiguration:
    case_id: str
    backend: str
    handler: str
    c1: int
    c2: int
    c3: int
    warmup_windows: int
    sample_windows: int
    request_frame_bytes: int | None = None
    request_payload_bytes: int | None = None
    response_payload_bytes: int | None = None
    stage_distribution: bool = False
    packet_handler: str = "empty"

    def __post_init__(self) -> None:
        if not self.case_id:
            raise ConfigurationError("case_id must not be empty")
        if self.backend not in ("dpdk", "roce"):
            raise ConfigurationError("backend must be dpdk or roce")
        if self.handler not in _PAYLOADS:
            raise ConfigurationError(f"unsupported handler {self.handler!r}")
        if self.packet_handler not in ("empty", "echo"):
            raise ConfigurationError("unsupported packet handler")
        if self.backend == "roce" and self.packet_handler != "empty":
            raise ConfigurationError("RoCE does not support a packet handler")
        if min(self.c1, self.c2, self.c3) <= 0 or self.c2 > self.c1:
            raise ConfigurationError("case requires C1 >= C2 > 0 and C3 > 0")
        if self.warmup_windows < 0 or self.sample_windows <= 0:
            raise ConfigurationError("invalid measurement windows")
        payload_values = (
            self.request_frame_bytes,
            self.request_payload_bytes,
            self.response_payload_bytes,
        )
        if any(value is not None for value in payload_values):
            if any(
                type(value) is not int or value <= 0
                for value in payload_values
            ):
                raise ConfigurationError(
                    "explicit frame and payload sizes must be positive integers"
                )


def common_overrides(case: CaseConfiguration) -> dict[str, object]:
    default_request, default_response = _PAYLOADS[case.handler]
    request_bytes = (
        default_request
        if case.request_payload_bytes is None
        else case.request_payload_bytes
    )
    response_bytes = (
        default_response
        if case.response_payload_bytes is None
        else case.response_payload_bytes
    )
    result: dict[str, object] = {
        "handler.apply_new_mbuf": case.handler == "file_read",
        "handler.message_handler": case.handler,
        "handler.packet_handler": "empty",
        "handler.request_payload_bytes": request_bytes,
        "handler.response_payload_bytes": response_bytes,
        "knobs.build.mempool_handler": (
            "huge_alloc" if case.backend == "roce" else "ring_mp_mc"
        ),
        "metrics.enabled": True,
        "metrics.human_output": False,
        "metrics.stage_distribution.enabled": case.stage_distribution,
        "network.backend": case.backend,
        "other.iterations": _trial_iterations(case),
        "tuning.sample_windows": case.sample_windows,
        "tuning.warmup_windows": case.warmup_windows,
    }
    if case.backend == "roce":
        result["other.mempool_cache_size"] = 0
    return result


def target_overrides(case: CaseConfiguration) -> dict[str, object]:
    result: dict[str, object] = {
        "knobs.runtime.application_core_count": case.c1,
        "knobs.runtime.dispatcher_queue_count": case.c2,
    }
    for name in (
        "app_rx_batch_size",
        "app_tx_batch_size",
        "dispatcher_rx_batch_size",
        "dispatcher_tx_batch_size",
        "nic_rx_post_size",
        "nic_tx_post_size",
    ):
        result[f"knobs.runtime.{name}"] = case.c3
    if case.packet_handler != "empty":
        result["handler.packet_handler"] = case.packet_handler
    return result


class ConfigMaterializer:
    def __init__(self, configure_binary: pathlib.Path) -> None:
        self._binary = configure_binary

    def _run(self, arguments: list[str]) -> None:
        completed = subprocess.run(
            [str(self._binary), *arguments],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise ConfigurationError(
                completed.stderr.strip() or "axio-configure failed"
            )

    @staticmethod
    def _payload(values: dict[str, object]) -> str:
        return json.dumps(values, allow_nan=False, separators=(",", ":"), sort_keys=True)

    def materialize(
        self,
        *,
        case: CaseConfiguration,
        target_input: pathlib.Path,
        peer_input: pathlib.Path,
        target_output: pathlib.Path,
        peer_output: pathlib.Path,
    ) -> None:
        target_output.parent.mkdir(parents=True, exist_ok=True)
        common_target = target_output.with_suffix(".common.toml")
        common_peer = peer_output.with_suffix(".common.toml")
        self._run(
            [
                "materialize-pair",
                str(target_input),
                str(peer_input),
                str(common_target),
                str(common_peer),
                "--set-json",
                self._payload(common_overrides(case)),
            ]
        )
        try:
            self._run(
                [
                    "materialize-target-pair",
                    str(common_target),
                    str(common_peer),
                    str(target_output),
                    str(peer_output),
                    "--target-set-json",
                    self._payload(target_overrides(case)),
                ]
            )
        finally:
            for path in (common_target, common_peer):
                path.unlink(missing_ok=True)
