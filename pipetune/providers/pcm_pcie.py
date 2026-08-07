"""Bounded PCM PCIe command generation and header-driven rate parsing."""

from __future__ import annotations

import csv
import io
import math

from pipetune.model import ContractError, ProviderStatus
from pipetune.providers import (
    ProviderResult,
    available_counter,
    unavailable_counter,
)


READ_COLUMNS = ("PCIeRdCur", "PCIRdCur")


class PcmPcieProvider:
    def __init__(self, path: str, version: str) -> None:
        if not path or not version:
            raise ContractError("pcm-pcie path and version must not be empty")
        self.path = path
        self.version = version

    @staticmethod
    def version_command() -> tuple[str, ...]:
        return ("env", "LC_ALL=C", "dpkg-query", "-W", "pcm")

    @staticmethod
    def parse_version(stdout: bytes) -> str:
        try:
            fields = stdout.decode("utf-8", errors="strict").strip().split()
        except UnicodeDecodeError as error:
            raise ValueError("pcm package version is not valid UTF-8") from error
        if len(fields) != 2 or fields[0] != "pcm":
            raise ValueError("dpkg-query returned an invalid pcm version")
        return f"pcm {fields[1]}"

    def command(
        self,
        *,
        output_path: str,
        sample_interval_seconds: float,
        period_seconds: float = 1.0,
    ) -> tuple[str, ...]:
        for value, name in (
            (sample_interval_seconds, "sample interval"),
            (period_seconds, "period"),
        ):
            if (
                type(value) not in (int, float)
                or not math.isfinite(value)
                or value <= 0
            ):
                raise ContractError(f"pcm-pcie {name} must be positive and finite")
        if not output_path:
            raise ContractError("pcm-pcie output path must not be empty")
        iterations = math.ceil(sample_interval_seconds / period_seconds)
        return (
            "env",
            "LC_ALL=C",
            self.path,
            format(float(period_seconds), "g"),
            "-e",
            f"-i={iterations}",
            f"-csv={output_path}",
        )

    def _status(self, *, available: bool, reason: str | None) -> ProviderStatus:
        return ProviderStatus(
            name="pcm_pcie",
            available=available,
            path=self.path,
            version=self.version if available else None,
            reason=reason,
        )

    def parse(
        self,
        raw_csv: bytes,
        *,
        stderr: bytes,
        socket_id: int,
    ) -> ProviderResult:
        names = ("io_read", "io_write")
        if type(socket_id) is not int or socket_id < 0:
            raise ContractError("pcm-pcie socket ID must be non-negative")
        try:
            error_text = stderr.decode("utf-8", errors="strict")
            csv_text = raw_csv.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            reason = "pcm-pcie output is not valid UTF-8"
            return ProviderResult(
                self._status(available=False, reason=reason),
                tuple(unavailable_counter(name, reason) for name in names),
            )
        lowered = error_text.casefold()
        if any(token in lowered for token in ("permission", "msr handle", "access cpus")):
            reason = "pcm-pcie permission or MSR failure"
            return ProviderResult(
                self._status(available=False, reason=reason),
                tuple(unavailable_counter(name, reason) for name in names),
            )
        try:
            blocks = _parse_blocks(csv_text)
            if not blocks:
                raise ValueError("pcm-pcie output is truncated")
            selected = []
            for block in blocks:
                if socket_id not in block:
                    raise LookupError(f"pcm-pcie socket {socket_id} is absent")
                rows = block[socket_id]
                if set(rows) != {"total", "miss", "hit"}:
                    raise ValueError("pcm-pcie output is truncated")
                selected.append(rows)
            for rows in selected:
                for index in (0, 1):
                    total = rows["total"][index]
                    miss = rows["miss"][index]
                    hit = rows["hit"][index]
                    if total != miss + hit:
                        raise ArithmeticError("pcm-pcie total/miss/hit are inconsistent")
            read_total = sum(rows["total"][0] for rows in selected)
            read_miss = sum(rows["miss"][0] for rows in selected)
            write_total = sum(rows["total"][1] for rows in selected)
            write_miss = sum(rows["miss"][1] for rows in selected)
            if read_total <= 0 or write_total <= 0:
                raise ZeroDivisionError("pcm-pcie counter has zero denominator")
        except LookupError as error:
            reason = str(error)
            return ProviderResult(
                self._status(available=True, reason=None),
                tuple(unavailable_counter(name, reason) for name in names),
            )
        except (ArithmeticError, ValueError, ZeroDivisionError) as error:
            reason = str(error)
            return ProviderResult(
                self._status(available=True, reason=None),
                tuple(unavailable_counter(name, reason) for name in names),
            )
        return ProviderResult(
            self._status(available=True, reason=None),
            (
                available_counter("io_read", read_miss, read_total),
                available_counter("io_write", write_miss, write_total),
            ),
        )


def _parse_blocks(
    text: str,
) -> list[dict[int, dict[str, tuple[int, int]]]]:
    blocks: list[dict[int, dict[str, tuple[int, int]]]] = []
    current: dict[int, dict[str, tuple[int, int]]] | None = None
    socket_index = -1
    read_index = -1
    write_index = -1
    for row in csv.reader(io.StringIO(text)):
        stripped = [field.strip() for field in row]
        read_columns = [name for name in READ_COLUMNS if name in stripped]
        if "Skt" in stripped and "ItoM" in stripped and read_columns:
            if current is not None:
                blocks.append(current)
            current = {}
            socket_index = stripped.index("Skt")
            read_index = stripped.index(read_columns[0])
            write_index = stripped.index("ItoM")
            continue
        if current is None or not stripped:
            continue
        label = _row_label(stripped)
        if label is None:
            continue
        try:
            socket = _integer(stripped[socket_index])
            read = _integer(stripped[read_index])
            write = _integer(stripped[write_index])
        except (IndexError, ValueError) as error:
            raise ValueError("pcm-pcie sample has invalid numeric fields") from error
        socket_rows = current.setdefault(socket, {})
        if label in socket_rows:
            raise ValueError("pcm-pcie output contains duplicate rows")
        socket_rows[label] = (read, write)
    if current is not None:
        blocks.append(current)
    return blocks


def _row_label(fields: list[str]) -> str | None:
    for field in fields:
        lowered = field.casefold()
        for label in ("total", "miss", "hit"):
            if lowered == label or lowered.endswith(f"({label})"):
                return label
    return None


def _integer(value: str) -> int:
    if not value or "," in value:
        raise ValueError("counter is not locale-stable")
    number = int(value, 10)
    if number < 0:
        raise ValueError("counter is negative")
    return number
