"""Small foreground worker used by local and SSH PipeTune transports."""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import pathlib
import signal
import shutil
import subprocess
import sys
import tempfile
import time
from typing import BinaryIO, Sequence


STATE_SCHEMA = "pipetune.worker-state/v1"
WORKLOAD_READY_SCHEMA = "pipetune.workload-ready/v1"


class WorkerStateError(RuntimeError):
    """Raised when a worker state file cannot safely identify its process."""


@dataclasses.dataclass(frozen=True)
class ProcessIdentity:
    pid: int
    pgid: int
    start_ticks: str
    argv_sha256: str


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _parse_proc_start_ticks(stat: str) -> str:
    command_end = stat.rfind(")")
    if command_end < 0:
        raise ProcessLookupError("malformed /proc stat")
    fields_after_command = stat[command_end + 1 :].split()
    if len(fields_after_command) < 20:
        raise ProcessLookupError("truncated /proc stat")
    return fields_after_command[19]


def _process_start_identity(pid: int) -> tuple[int, str]:
    proc = pathlib.Path("/proc") / str(pid)
    try:
        stat = (proc / "stat").read_text(encoding="utf-8")
    except FileNotFoundError:
        pgid = os.getpgid(pid)
        return pgid, f"portable-pgid:{pgid}"
    return os.getpgid(pid), _parse_proc_start_ticks(stat)


def _process_bytes(pid: int) -> tuple[bytes, int, str]:
    pgid, start_ticks = _process_start_identity(pid)
    if start_ticks.startswith("portable-pgid:"):
        command = f"pid={pid};pgid={pgid}".encode("ascii")
        return command, pgid, start_ticks

    try:
        command = (pathlib.Path("/proc") / str(pid) / "cmdline").read_bytes()
    except FileNotFoundError:
        command = b""
    if not command:
        command = f"pid={pid};pgid={pgid};cmdline-unavailable".encode("ascii")
    return command, pgid, start_ticks


def process_identity(pid: int) -> ProcessIdentity:
    command, pgid, start_ticks = _process_bytes(pid)
    return ProcessIdentity(
        pid=pid,
        pgid=pgid,
        start_ticks=start_ticks,
        argv_sha256=_sha256_bytes(command),
    )


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


def write_state(path: pathlib.Path, document: dict[str, object]) -> None:
    payload = (
        json.dumps(document, allow_nan=False, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    _write_bytes_atomic(path, payload)


def _load_state(path: pathlib.Path) -> dict[str, object]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise WorkerStateError(f"cannot read worker state {path}: {error}") from error
    if not isinstance(document, dict):
        raise WorkerStateError(f"worker state {path} must be an object")
    expected = {
        "argv_sha256",
        "pgid",
        "pid",
        "schema",
        "session_id",
        "start_ticks",
        "workload_pgid",
        "workload_pid",
        "workload_start_ticks",
    }
    if set(document) != expected:
        raise WorkerStateError(f"worker state {path} has invalid fields")
    if document["schema"] != STATE_SCHEMA:
        raise WorkerStateError(f"worker state {path} has unsupported schema")
    return document


def _load_workload_ready(path: pathlib.Path) -> tuple[int, int, str]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise WorkerStateError(
            f"cannot read workload readiness {path}: {error}"
        ) from error
    if not isinstance(document, dict) or set(document) != {
        "pgid",
        "pid",
        "schema",
        "start_ticks",
    }:
        raise WorkerStateError("workload readiness has invalid fields")
    if document["schema"] != WORKLOAD_READY_SCHEMA:
        raise WorkerStateError("workload readiness has unsupported schema")
    pid = document["pid"]
    pgid = document["pgid"]
    start_ticks = document["start_ticks"]
    if type(pid) is not int or type(pgid) is not int or pid <= 0 or pgid <= 0:
        raise WorkerStateError("workload PID and PGID must be positive integers")
    if not isinstance(start_ticks, str) or not start_ticks:
        raise WorkerStateError("workload start_ticks must be a string")
    return pid, pgid, start_ticks


def _state_identity(document: dict[str, object]) -> ProcessIdentity:
    pid = document["pid"]
    pgid = document["pgid"]
    start_ticks = document["start_ticks"]
    argv_sha256 = document["argv_sha256"]
    if type(pid) is not int or type(pgid) is not int or pid <= 0 or pgid <= 0:
        raise WorkerStateError("worker state PID and PGID must be positive integers")
    if not isinstance(start_ticks, str) or not start_ticks:
        raise WorkerStateError("worker state start_ticks must be a string")
    if not isinstance(argv_sha256, str) or len(argv_sha256) != 64:
        raise WorkerStateError("worker state argv_sha256 must be SHA-256")
    return ProcessIdentity(pid, pgid, start_ticks, argv_sha256)


def _state_workload(document: dict[str, object]) -> tuple[int, int, str]:
    pid = document["workload_pid"]
    pgid = document["workload_pgid"]
    start_ticks = document["workload_start_ticks"]
    if type(pid) is not int or type(pgid) is not int or pid <= 0 or pgid <= 0:
        raise WorkerStateError("worker workload PID and PGID must be positive integers")
    if not isinstance(start_ticks, str) or not start_ticks:
        raise WorkerStateError("worker workload start_ticks must be a string")
    return pid, pgid, start_ticks


def _same_process(expected: ProcessIdentity) -> bool:
    if expected.pid != expected.pgid:
        return False
    try:
        current_pgid, current_start_ticks = _process_start_identity(expected.pid)
    except (OSError, ProcessLookupError):
        return False
    return (
        current_pgid == expected.pgid
        and current_start_ticks == expected.start_ticks
    )


def _same_workload(document: dict[str, object]) -> bool:
    pid, pgid, start_ticks = _state_workload(document)
    try:
        current_pgid, current_start_ticks = _process_start_identity(pid)
    except (OSError, ProcessLookupError):
        return False
    return current_pgid == pgid and current_start_ticks == start_ticks


def exec_workload(ready_path: pathlib.Path, argv: Sequence[str]) -> None:
    """Publish the privileged workload identity, then replace this wrapper."""

    if not argv or any(not isinstance(argument, str) for argument in argv):
        raise WorkerStateError("workload argv must contain strings")
    identity = process_identity(os.getpid())
    write_state(
        ready_path,
        {
            "pgid": identity.pgid,
            "pid": identity.pid,
            "schema": WORKLOAD_READY_SCHEMA,
            "start_ticks": identity.start_ticks,
        },
    )
    os.execvp(argv[0], list(argv))


def _new_ready_path(state_path: pathlib.Path) -> pathlib.Path:
    state_path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(
        prefix=f".{state_path.name}.", suffix=".workload-ready", dir=state_path.parent
    )
    os.close(descriptor)
    path = pathlib.Path(name)
    path.unlink()
    return path


def _wait_for_workload(
    child: subprocess.Popen, ready_path: pathlib.Path, launcher: ProcessIdentity
) -> tuple[int, int, str] | None:
    deadline = time.monotonic() + 10.0
    while child.poll() is None and time.monotonic() < deadline:
        if ready_path.exists():
            workload = _load_workload_ready(ready_path)
            if workload[1] != launcher.pgid:
                raise WorkerStateError(
                    "sudo workload escaped the managed process group"
                )
            return workload
        time.sleep(0.01)
    if ready_path.exists():
        workload = _load_workload_ready(ready_path)
        if workload[1] != launcher.pgid:
            raise WorkerStateError("sudo workload escaped the managed process group")
        return workload
    if child.poll() is not None:
        return None
    raise WorkerStateError("sudo workload did not publish its process identity")


def terminate_session(
    state_path: pathlib.Path,
    session_id: str,
    *,
    grace_seconds: float = 2.0,
) -> str:
    if not state_path.exists():
        return "already_stopped"
    document = _load_state(state_path)
    if document["session_id"] != session_id:
        raise WorkerStateError("worker state belongs to another session")
    expected = _state_identity(document)
    if not _same_process(expected):
        state_path.unlink(missing_ok=True)
        return "stale_removed"

    try:
        os.killpg(expected.pgid, signal.SIGTERM)
    except ProcessLookupError:
        state_path.unlink(missing_ok=True)
        return "already_stopped"
    deadline = time.monotonic() + max(grace_seconds, 0.0)
    while time.monotonic() < deadline:
        if not _same_process(expected):
            state_path.unlink(missing_ok=True)
            return "terminated"
        time.sleep(0.01)
    if _same_process(expected):
        try:
            os.killpg(expected.pgid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    state_path.unlink(missing_ok=True)
    return "terminated"


def _remove_owned_state(
    state_path: pathlib.Path, session_id: str, identity: ProcessIdentity
) -> None:
    try:
        document = _load_state(state_path)
        stored = _state_identity(document)
    except (FileNotFoundError, WorkerStateError):
        return
    if document["session_id"] == session_id and stored == identity:
        state_path.unlink(missing_ok=True)


def run_foreground(
    *,
    state_path: pathlib.Path,
    session_id: str,
    cwd: pathlib.Path,
    stdout_path: pathlib.Path,
    stderr_path: pathlib.Path,
    argv: Sequence[str],
    use_sudo: bool,
    sudo_program: str = "sudo",
) -> dict[str, object]:
    if not argv or any(not isinstance(argument, str) for argument in argv):
        raise WorkerStateError("command argv must contain strings")
    if state_path.exists():
        raise WorkerStateError(f"worker state already exists: {state_path}")
    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    stderr_path.parent.mkdir(parents=True, exist_ok=True)
    ready_path = _new_ready_path(state_path) if use_sudo else None
    command = list(argv)
    if use_sudo:
        assert ready_path is not None
        command = [
            sudo_program,
            "-n",
            "--",
            sys.executable,
            str(pathlib.Path(__file__).resolve()),
            "exec-workload",
            "--ready",
            str(ready_path),
            "--",
            *argv,
        ]
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        child = subprocess.Popen(
            command,
            cwd=cwd,
            stdout=stdout,
            stderr=stderr,
            start_new_session=True,
        )
        try:
            identity = process_identity(child.pid)
        except ProcessLookupError:
            return {"return_code": child.wait(), "status": "exited"}
        if identity.pid != identity.pgid:
            child.kill()
            child.wait()
            raise WorkerStateError("child did not create a private process group")
        try:
            workload = (
                _wait_for_workload(child, ready_path, identity)
                if ready_path is not None
                else (identity.pid, identity.pgid, identity.start_ticks)
            )
        except BaseException:
            try:
                os.killpg(identity.pgid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait()
            raise
        finally:
            if ready_path is not None:
                ready_path.unlink(missing_ok=True)
        if workload is None:
            return {"return_code": child.wait(), "status": "exited"}
        write_state(
            state_path,
            {
                "argv_sha256": identity.argv_sha256,
                "pgid": identity.pgid,
                "pid": identity.pid,
                "schema": STATE_SCHEMA,
                "session_id": session_id,
                "start_ticks": identity.start_ticks,
                "workload_pgid": workload[1],
                "workload_pid": workload[0],
                "workload_start_ticks": workload[2],
            },
        )

        previous_handlers: dict[int, object] = {}

        def forward(signum: int, _frame: object) -> None:
            try:
                os.killpg(identity.pgid, signum)
            except ProcessLookupError:
                pass

        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            previous_handlers[signum] = signal.signal(signum, forward)
        try:
            return {"return_code": child.wait(), "status": "exited"}
        finally:
            for signum, handler in previous_handlers.items():
                signal.signal(signum, handler)
            _remove_owned_state(state_path, session_id, identity)


def receive_file(
    source: BinaryIO, destination: pathlib.Path, expected_sha256: str
) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", suffix=".tmp", dir=destination.parent
    )
    temporary = pathlib.Path(temporary_name)
    digest = hashlib.sha256()
    try:
        with os.fdopen(descriptor, "wb") as output:
            while True:
                chunk = source.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
                output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        if digest.hexdigest() != expected_sha256:
            raise WorkerStateError("uploaded file SHA-256 mismatch")
        os.replace(temporary, destination)
        if hasattr(os, "O_DIRECTORY"):
            directory = os.open(destination.parent, os.O_RDONLY | os.O_DIRECTORY)
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


def remove_contained_tree(path: pathlib.Path, containment_root: pathlib.Path) -> str:
    root = containment_root.resolve(strict=False)
    target = path.resolve(strict=False)
    if target == root:
        raise WorkerStateError("refusing to remove the containment root")
    try:
        target.relative_to(root)
    except ValueError as error:
        raise WorkerStateError("cleanup path escapes its containment root") from error
    if path.is_symlink():
        raise WorkerStateError("refusing to remove a symlink cleanup path")
    if not path.exists():
        return "already_removed"
    if not containment_root.is_dir():
        raise WorkerStateError("cleanup containment root must be a directory")
    if not path.is_dir():
        raise WorkerStateError("cleanup path must be a directory")
    shutil.rmtree(path)
    return "removed"


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="pipetune-remote-worker")
    subparsers = parser.add_subparsers(dest="operation", required=True)
    run = subparsers.add_parser("run")
    run.add_argument("--state", required=True)
    run.add_argument("--session", required=True)
    run.add_argument("--cwd", required=True)
    run.add_argument("--stdout", required=True)
    run.add_argument("--stderr", required=True)
    run.add_argument("--sudo", action="store_true")
    run.add_argument("--sudo-program", default="sudo")
    run.add_argument("argv", nargs=argparse.REMAINDER)
    exec_parser = subparsers.add_parser("exec-workload")
    exec_parser.add_argument("--ready", required=True)
    exec_parser.add_argument("argv", nargs=argparse.REMAINDER)
    terminate = subparsers.add_parser("terminate")
    terminate.add_argument("--state", required=True)
    terminate.add_argument("--session", required=True)
    inspect = subparsers.add_parser("inspect")
    inspect.add_argument("--state", required=True)
    inspect.add_argument("--session", required=True)
    put = subparsers.add_parser("put")
    put.add_argument("--destination", required=True)
    put.add_argument("--sha256", required=True)
    get = subparsers.add_parser("get")
    get.add_argument("--source", required=True)
    remove_tree = subparsers.add_parser("remove-tree")
    remove_tree.add_argument("--path", required=True)
    remove_tree.add_argument("--containment-root", required=True)
    return parser


def _run_cli(arguments: argparse.Namespace) -> dict[str, object] | None:
    if arguments.operation == "run":
        argv = arguments.argv
        if argv and argv[0] == "--":
            argv = argv[1:]
        return run_foreground(
            state_path=pathlib.Path(arguments.state),
            session_id=arguments.session,
            cwd=pathlib.Path(arguments.cwd),
            stdout_path=pathlib.Path(arguments.stdout),
            stderr_path=pathlib.Path(arguments.stderr),
            argv=argv,
            use_sudo=arguments.sudo,
            sudo_program=arguments.sudo_program,
        )
    if arguments.operation == "exec-workload":
        argv = arguments.argv
        if argv and argv[0] == "--":
            argv = argv[1:]
        exec_workload(pathlib.Path(arguments.ready), argv)
        raise WorkerStateError("workload exec unexpectedly returned")
    if arguments.operation == "terminate":
        return {
            "status": terminate_session(
                pathlib.Path(arguments.state), arguments.session
            )
        }
    if arguments.operation == "inspect":
        path = pathlib.Path(arguments.state)
        if not path.exists():
            return {"status": "missing"}
        document = _load_state(path)
        if document["session_id"] != arguments.session:
            raise WorkerStateError("worker state belongs to another session")
        running = _same_process(_state_identity(document)) and _same_workload(document)
        return {"status": "running" if running else "stale"}
    if arguments.operation == "put":
        receive_file(
            sys.stdin.buffer,
            pathlib.Path(arguments.destination),
            arguments.sha256,
        )
        return {"status": "stored"}
    if arguments.operation == "get":
        with pathlib.Path(arguments.source).open("rb") as source:
            while True:
                chunk = source.read(1024 * 1024)
                if not chunk:
                    break
                sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
        return None
    if arguments.operation == "remove-tree":
        return {
            "status": remove_contained_tree(
                pathlib.Path(arguments.path),
                pathlib.Path(arguments.containment_root),
            )
        }
    raise WorkerStateError(f"unsupported operation {arguments.operation}")


def main(argv: Sequence[str] | None = None) -> int:
    try:
        result = _run_cli(_parser().parse_args(argv))
        if result is not None:
            print(json.dumps(result, allow_nan=False, sort_keys=True))
        return 0
    except (OSError, ValueError, WorkerStateError) as error:
        print(f"pipetune-remote-worker: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
