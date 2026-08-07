"""Controller-neutral local and system-SSH process/file transport."""

from __future__ import annotations

import dataclasses
import datetime as dt
import hashlib
import json
import os
import pathlib
import shlex
import subprocess
import sys
import tempfile
import time
from typing import Any, Sequence

from pipetune.model import ContractError, EndpointSpec


class TransportError(RuntimeError):
    """Raised when a controller transport operation cannot complete safely."""


@dataclasses.dataclass(frozen=True)
class ResolvedEndpoint:
    spec: EndpointSpec
    config_path: pathlib.Path
    binary_path: str


@dataclasses.dataclass(frozen=True)
class CommandOutcome:
    argv: tuple[str, ...]
    status: str
    return_code: int | None
    failure_reason: str | None
    started_at_utc: str
    ended_at_utc: str


def _utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def _unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ContractError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise ContractError(f"non-finite JSON constant {value}")


def _require_object(value: object, location: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ContractError(f"{location} must be an object")
    return value


def resolve_endpoint(
    *,
    endpoint_id: str,
    config_path: pathlib.Path,
    configure_binary: pathlib.Path,
    binary_override: str | None = None,
) -> ResolvedEndpoint:
    try:
        completed = subprocess.run(
            [str(configure_binary), "dump", str(config_path)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as error:
        raise TransportError(f"cannot execute {configure_binary}: {error}") from error
    if completed.returncode != 0:
        message = completed.stderr.strip() or "axio-configure dump failed"
        raise TransportError(message)
    try:
        document = json.loads(
            completed.stdout,
            parse_constant=_reject_constant,
            object_pairs_hook=_unique_object,
        )
    except json.JSONDecodeError as error:
        raise ContractError(f"invalid axio-configure dump: {error}") from error
    root = _require_object(document, "config dump")
    deployment = _require_object(root.get("deployment"), "deployment")
    network = _require_object(root.get("network"), "network")
    required = {
        "host",
        "numa_node",
        "role",
        "ssh_port",
        "ssh_user",
        "transport",
        "use_sudo",
        "workdir",
    }
    missing = sorted(required - deployment.keys())
    if missing:
        raise ContractError(f"deployment is missing {missing}")
    spec = EndpointSpec(
        endpoint_id=endpoint_id,
        role=deployment["role"],
        backend=network.get("backend"),
        transport=deployment["transport"],
        host=deployment["host"],
        ssh_port=deployment["ssh_port"],
        ssh_user=deployment["ssh_user"],
        workdir=deployment["workdir"],
        use_sudo=deployment["use_sudo"],
        numa_node=deployment["numa_node"],
    )
    if binary_override is not None:
        if not binary_override:
            raise ContractError("binary override must not be empty")
        binary_path = binary_override
    else:
        binary_path = str(
            pathlib.PurePosixPath(spec.workdir) / f"build-{spec.role}" / "axio"
        )
    return ResolvedEndpoint(spec, config_path.resolve(), binary_path)


def _write_bytes_atomic(path: pathlib.Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
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
        try:
            os.close(descriptor)
        except OSError:
            pass
        temporary.unlink(missing_ok=True)
        raise


class ManagedProcess:
    def __init__(
        self,
        *,
        transport: "WorkerTransport",
        launcher: subprocess.Popen,
        session_id: str,
        state_path: str,
        argv: tuple[str, ...],
        started_at_utc: str,
    ) -> None:
        self._transport = transport
        self._launcher = launcher
        self._session_id = session_id
        self._state_path = state_path
        self._argv = argv
        self._started_at_utc = started_at_utc
        self._outcome: CommandOutcome | None = None

    def _transport_failure(self, stderr: bytes) -> CommandOutcome:
        reason = stderr.decode("utf-8", errors="replace").strip()
        return CommandOutcome(
            argv=self._argv,
            status="transport_error",
            return_code=None,
            failure_reason=reason or "worker transport closed without a result",
            started_at_utc=self._started_at_utc,
            ended_at_utc=_utc_now(),
        )

    def wait(self, *, timeout_seconds: float | None = None) -> CommandOutcome:
        if self._outcome is not None:
            return self._outcome
        try:
            stdout, stderr = self._launcher.communicate(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            try:
                self._transport.terminate(self._state_path, self._session_id)
            finally:
                try:
                    self._launcher.communicate(timeout=5.0)
                except subprocess.TimeoutExpired:
                    self._launcher.kill()
                    self._launcher.communicate()
            self._outcome = CommandOutcome(
                argv=self._argv,
                status="timed_out",
                return_code=None,
                failure_reason="command exceeded its timeout",
                started_at_utc=self._started_at_utc,
                ended_at_utc=_utc_now(),
            )
            return self._outcome
        if self._launcher.returncode != 0:
            self._outcome = self._transport_failure(stderr)
            return self._outcome
        try:
            document = json.loads(stdout)
            return_code = document["return_code"]
            if document.get("status") != "exited" or type(return_code) is not int:
                raise ValueError("invalid worker result")
        except (json.JSONDecodeError, KeyError, TypeError, ValueError) as error:
            self._outcome = self._transport_failure(
                stderr + f"\ninvalid worker result: {error}".encode()
            )
            return self._outcome
        self._outcome = CommandOutcome(
            argv=self._argv,
            status="exited",
            return_code=return_code,
            failure_reason=None,
            started_at_utc=self._started_at_utc,
            ended_at_utc=_utc_now(),
        )
        return self._outcome

    def terminate(self) -> CommandOutcome:
        if self._outcome is None and self._launcher.poll() is None:
            self._transport.terminate(self._state_path, self._session_id)
        return self.wait(timeout_seconds=5.0)


class WorkerTransport:
    def _worker_argv(self, arguments: Sequence[str]) -> list[str]:
        raise NotImplementedError

    def _popen(self, arguments: Sequence[str]) -> subprocess.Popen:
        raise NotImplementedError

    def _control(
        self,
        arguments: Sequence[str],
        *,
        input_bytes: bytes | None = None,
        timeout_seconds: float = 10.0,
    ) -> subprocess.CompletedProcess:
        raise NotImplementedError

    def start(
        self,
        *,
        session_id: str,
        argv: Sequence[str],
        cwd: pathlib.Path | str,
        state_path: pathlib.Path | str,
        stdout_path: pathlib.Path | str,
        stderr_path: pathlib.Path | str,
        use_sudo: bool = False,
        ready_timeout_seconds: float = 5.0,
    ) -> ManagedProcess:
        command = tuple(argv)
        if not command or any(not isinstance(argument, str) for argument in command):
            raise ContractError("command argv must contain strings")
        state = str(state_path)
        worker_arguments = [
            "run",
            "--state",
            state,
            "--session",
            session_id,
            "--cwd",
            str(cwd),
            "--stdout",
            str(stdout_path),
            "--stderr",
            str(stderr_path),
        ]
        if use_sudo:
            worker_arguments.append("--sudo")
        worker_arguments.extend(("--", *command))
        launcher = self._popen(self._worker_argv(worker_arguments))
        handle = ManagedProcess(
            transport=self,
            launcher=launcher,
            session_id=session_id,
            state_path=state,
            argv=command,
            started_at_utc=_utc_now(),
        )
        deadline = time.monotonic() + ready_timeout_seconds
        while launcher.poll() is None and time.monotonic() < deadline:
            remaining = max(deadline - time.monotonic(), 0.01)
            try:
                result = self._control(
                    self._worker_argv(
                        ["inspect", "--state", state, "--session", session_id]
                    ),
                    timeout_seconds=remaining,
                )
            except TransportError:
                launcher.kill()
                launcher.communicate()
                raise
            if result.returncode == 0:
                try:
                    if json.loads(result.stdout).get("status") == "running":
                        return handle
                except (json.JSONDecodeError, AttributeError):
                    pass
            time.sleep(0.01)
        if launcher.poll() is None:
            launcher.kill()
            launcher.communicate()
            raise TransportError("worker did not publish a valid process state")
        return handle

    def terminate(self, state_path: str, session_id: str) -> str:
        result = self._control(
            self._worker_argv(
                [
                    "terminate",
                    "--state",
                    state_path,
                    "--session",
                    session_id,
                ]
            )
        )
        if result.returncode != 0:
            raise TransportError(result.stderr.decode(errors="replace").strip())
        try:
            return json.loads(result.stdout)["status"]
        except (json.JSONDecodeError, KeyError, TypeError) as error:
            raise TransportError("terminate returned invalid worker JSON") from error


class LocalTransport(WorkerTransport):
    def __init__(self, *, worker_path: pathlib.Path) -> None:
        self._worker_path = worker_path.resolve()

    def _worker_argv(self, arguments: Sequence[str]) -> list[str]:
        return [sys.executable, str(self._worker_path), *arguments]

    def _popen(self, arguments: Sequence[str]) -> subprocess.Popen:
        return subprocess.Popen(
            list(arguments),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def _control(
        self,
        arguments: Sequence[str],
        *,
        input_bytes: bytes | None = None,
        timeout_seconds: float = 10.0,
    ) -> subprocess.CompletedProcess:
        return subprocess.run(
            list(arguments),
            input=input_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_seconds,
            check=False,
        )

    def upload(self, source: pathlib.Path, destination: str) -> None:
        try:
            _write_bytes_atomic(pathlib.Path(destination), source.read_bytes())
        except OSError as error:
            raise TransportError(f"local upload failed: {error}") from error

    def download(self, source: str, destination: pathlib.Path) -> None:
        try:
            _write_bytes_atomic(destination, pathlib.Path(source).read_bytes())
        except OSError as error:
            raise TransportError(f"local download failed: {error}") from error


class SshTransport(WorkerTransport):
    def __init__(
        self,
        *,
        spec: EndpointSpec,
        worker_path: str,
        ssh_program: str = "ssh",
    ) -> None:
        if spec.transport != "ssh":
            raise ContractError("SshTransport requires an SSH endpoint")
        self._spec = spec
        self._worker_path = worker_path
        self._ssh_program = ssh_program

    def _worker_argv(self, arguments: Sequence[str]) -> list[str]:
        remote = ["python3", self._worker_path, *arguments]
        return [
            self._ssh_program,
            "-o",
            "BatchMode=yes",
            "-p",
            str(self._spec.ssh_port),
            f"{self._spec.ssh_user}@{self._spec.host}",
            shlex.join(remote),
        ]

    def _popen(self, arguments: Sequence[str]) -> subprocess.Popen:
        return subprocess.Popen(
            list(arguments),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def _control(
        self,
        arguments: Sequence[str],
        *,
        input_bytes: bytes | None = None,
        timeout_seconds: float = 10.0,
    ) -> subprocess.CompletedProcess:
        try:
            return subprocess.run(
                list(arguments),
                input=input_bytes,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=timeout_seconds,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired) as error:
            raise TransportError(f"SSH transport failed: {error}") from error

    def upload(self, source: pathlib.Path, destination: str) -> None:
        try:
            payload = source.read_bytes()
        except OSError as error:
            raise TransportError(f"cannot read upload source {source}: {error}") from error
        result = self._control(
            self._worker_argv(
                [
                    "put",
                    "--destination",
                    destination,
                    "--sha256",
                    hashlib.sha256(payload).hexdigest(),
                ]
            ),
            input_bytes=payload,
        )
        if result.returncode != 0:
            raise TransportError(result.stderr.decode(errors="replace").strip())
        try:
            if json.loads(result.stdout).get("status") != "stored":
                raise ValueError("unexpected status")
        except (json.JSONDecodeError, AttributeError, ValueError) as error:
            raise TransportError("upload returned invalid worker JSON") from error

    def download(self, source: str, destination: pathlib.Path) -> None:
        result = self._control(
            self._worker_argv(["get", "--source", source])
        )
        if result.returncode != 0:
            raise TransportError(result.stderr.decode(errors="replace").strip())
        _write_bytes_atomic(destination, result.stdout)


def transport_for(
    spec: EndpointSpec,
    *,
    worker_path: str | pathlib.Path | None = None,
    ssh_program: str = "ssh",
) -> WorkerTransport:
    if spec.transport == "local":
        local_worker = (
            pathlib.Path(worker_path)
            if worker_path is not None
            else pathlib.Path(__file__).with_name("remote_worker.py")
        )
        return LocalTransport(worker_path=local_worker)
    remote_worker = (
        str(worker_path)
        if worker_path is not None
        else str(
            pathlib.PurePosixPath(spec.workdir)
            / "pipetune"
            / "remote_worker.py"
        )
    )
    return SshTransport(
        spec=spec,
        worker_path=remote_worker,
        ssh_program=ssh_program,
    )
