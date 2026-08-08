from __future__ import annotations

import dataclasses
import hashlib
import json
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

from pipetune.model import ContractError, EndpointSpec
from pipetune.remote import (
    EndpointTransport,
    LocalTransport,
    SshTransport,
    TransportError,
    resolve_endpoint,
    transport_for,
)
from pipetune.remote_worker import (
    ProcessIdentity,
    WorkerStateError,
    _parse_proc_start_ticks,
    _same_process,
    exec_workload,
    process_identity,
    receive_file,
    run_foreground,
    terminate_session,
    write_state,
)


ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKER = ROOT / "pipetune/remote_worker.py"


def write_executable(path: pathlib.Path, source: str) -> None:
    path.write_text(source, encoding="utf-8")
    path.chmod(0o755)


def ssh_endpoint(workdir: pathlib.Path) -> EndpointSpec:
    return EndpointSpec(
        endpoint_id="target",
        role="client",
        backend="dpdk",
        transport="ssh",
        host="test-host",
        ssh_port=2222,
        ssh_user="tester",
        workdir=str(workdir),
        use_sudo=False,
        numa_node=1,
    )


class RemoteTransportTest(unittest.TestCase):
    def test_transport_factory_uses_deployment_mode(self) -> None:
        remote = ssh_endpoint(pathlib.Path("/remote/axio"))
        ssh_transport = transport_for(remote)
        self.assertIsInstance(ssh_transport, SshTransport)
        self.assertIsInstance(ssh_transport, EndpointTransport)
        local = dataclasses.replace(
            remote,
            transport="local",
            host="",
            ssh_port=0,
            ssh_user="",
            workdir=".",
        )
        local_transport = transport_for(local)
        self.assertIsInstance(local_transport, LocalTransport)
        self.assertIsInstance(local_transport, EndpointTransport)

    def test_resolve_endpoint_uses_dump_and_binary_override(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune resolve ; ") as temp_dir:
            root = pathlib.Path(temp_dir)
            configure = root / "axio configure"
            document = {
                "deployment": {
                    "host": "rDesktop_01",
                    "numa_node": 1,
                    "role": "client",
                    "ssh_port": 22,
                    "ssh_user": "ubuntu",
                    "transport": "ssh",
                    "use_sudo": True,
                    "workdir": "/home/ubuntu/axio tree",
                },
                "network": {"backend": "roce"},
            }
            write_executable(
                configure,
                "#!/usr/bin/env python3\n"
                "import json\n"
                f"print(json.dumps({document!r}))\n",
            )
            config = root / "client $(touch injected).toml"
            config.write_text("unused\n", encoding="utf-8")

            resolved = resolve_endpoint(
                endpoint_id="target",
                config_path=config,
                configure_binary=configure,
            )
            self.assertEqual(resolved.spec.host, "rDesktop_01")
            self.assertEqual(resolved.spec.backend, "roce")
            self.assertEqual(
                resolved.binary_path,
                "/home/ubuntu/axio tree/build-client/axio",
            )
            overridden = resolve_endpoint(
                endpoint_id="target",
                config_path=config,
                configure_binary=configure,
                binary_override="custom bin/axio",
            )
            self.assertEqual(overridden.binary_path, "custom bin/axio")
            self.assertFalse((root / "injected").exists())

    def test_local_process_lifecycle_and_timeout_are_session_scoped(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-local-") as temp_dir:
            root = pathlib.Path(temp_dir)
            transport = LocalTransport(worker_path=WORKER)
            stdout = root / "stdout file"
            stderr = root / "stderr file"
            state = root / "state file.json"
            literal = "literal ; $(touch injected)"
            handle = transport.start(
                session_id="local-success",
                argv=(sys.executable, "-c", "import sys; print(sys.argv[1])", literal),
                cwd=root,
                state_path=state,
                stdout_path=stdout,
                stderr_path=stderr,
            )
            outcome = transport.wait(handle, timeout_seconds=5.0)
            self.assertEqual((outcome.status, outcome.return_code), ("exited", 0))
            self.assertEqual(stdout.read_text().strip(), literal)
            self.assertFalse((root / "injected").exists())
            payload_path = root / "payload"
            transport.put_bytes(str(payload_path), b"payload")
            self.assertEqual(transport.get_bytes(str(payload_path)), b"payload")
            with self.assertRaises(ContractError):
                LocalTransport(worker_path=WORKER).wait(handle)

            failed = transport.run(
                session_id="local-failed",
                argv=(sys.executable, "-c", "raise SystemExit(17)"),
                cwd=root,
                state_path=state,
                stdout_path=stdout,
                stderr_path=stderr,
                timeout_seconds=5.0,
            )
            self.assertEqual((failed.status, failed.return_code), ("exited", 17))

            terminate_handle = transport.start(
                session_id="local-terminate",
                argv=(sys.executable, "-c", "import time; time.sleep(30)"),
                cwd=root,
                state_path=state,
                stdout_path=stdout,
                stderr_path=stderr,
            )
            terminated = transport.terminate(terminate_handle)
            self.assertEqual(terminated.status, "exited")
            self.assertLess(terminated.return_code or 0, 0)

            sentinel = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(30)"],
                start_new_session=True,
            )
            try:
                timed_out_handle = transport.start(
                    session_id="local-timeout",
                    argv=(sys.executable, "-c", "import time; time.sleep(30)"),
                    cwd=root,
                    state_path=state,
                    stdout_path=stdout,
                    stderr_path=stderr,
                )
                self.assertTrue(transport.is_running(timed_out_handle))
                timed_out = transport.wait(
                    timed_out_handle, timeout_seconds=0.1
                )
                self.assertEqual(timed_out.status, "timed_out")
                self.assertIsNone(timed_out.return_code)
                self.assertIsNone(sentinel.poll())
                self.assertFalse(state.exists())
                self.assertFalse(transport.is_running(timed_out_handle))
            finally:
                sentinel.terminate()
                sentinel.wait(timeout=5)

    def test_ssh_boundary_round_trips_metacharacter_paths(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune ssh ; ") as temp_dir:
            root = pathlib.Path(temp_dir)
            fake_ssh = root / "ssh double"
            write_executable(
                fake_ssh,
                "#!/usr/bin/env python3\n"
                "import os, sys\n"
                "os.execv('/bin/sh', ['sh', '-c', sys.argv[-1]])\n",
            )
            transport = SshTransport(
                spec=ssh_endpoint(root),
                worker_path=str(WORKER),
                ssh_program=str(fake_ssh),
            )
            source = root / "source ; $(touch source-injected)"
            remote = root / "remote ; $(touch remote-injected)"
            downloaded = root / "downloaded ; $(touch download-injected)"
            source.write_bytes(b"pipe tune\x00bytes\n")

            transport.put_bytes(str(remote), source.read_bytes())
            self.assertEqual(transport.get_bytes(str(remote)), source.read_bytes())
            state = root / "ssh state.json"
            stdout = root / "ssh stdout"
            stderr = root / "ssh stderr"
            literal = "remote literal ; $(touch process-injected)"
            handle = transport.start(
                session_id="ssh-success",
                argv=(sys.executable, "-c", "import sys; print(sys.argv[1])", literal),
                cwd=root,
                state_path=str(state),
                stdout_path=str(stdout),
                stderr_path=str(stderr),
            )
            outcome = transport.wait(handle, timeout_seconds=5.0)
            self.assertEqual((outcome.status, outcome.return_code), ("exited", 0))
            self.assertEqual(stdout.read_text().strip(), literal)
            for marker in (
                "source-injected",
                "remote-injected",
                "download-injected",
                "process-injected",
            ):
                self.assertFalse((root / marker).exists())

    def test_ssh_readiness_allows_proxy_connection_latency(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-slow-ssh-") as temp_dir:
            root = pathlib.Path(temp_dir)
            fake_ssh = root / "slow-ssh"
            write_executable(
                fake_ssh,
                "#!/usr/bin/env python3\n"
                "import os, sys, time\n"
                "time.sleep(1.1)\n"
                "os.execv('/bin/sh', ['sh', '-c', sys.argv[-1]])\n",
            )
            transport = SshTransport(
                spec=ssh_endpoint(root),
                worker_path=str(WORKER),
                ssh_program=str(fake_ssh),
            )
            handle = transport.start(
                session_id="slow-proxy",
                argv=(sys.executable, "-c", "import time; time.sleep(2)"),
                cwd=root,
                state_path=str(root / "state.json"),
                stdout_path=str(root / "stdout"),
                stderr_path=str(root / "stderr"),
                ready_timeout_seconds=3.0,
            )
            outcome = transport.wait(handle, timeout_seconds=5.0)
            self.assertEqual((outcome.status, outcome.return_code), ("exited", 0))

    def test_proc_stat_parser_handles_spaces_in_command_name(self) -> None:
        stat = "123 (worker with spaces) S " + " ".join(str(i) for i in range(4, 53))
        self.assertEqual(_parse_proc_start_ticks(stat), "22")

    def test_argv_digest_is_not_kill_identity(self) -> None:
        expected = ProcessIdentity(17, 17, "1234", "a" * 64)
        with mock.patch(
            "pipetune.remote_worker._process_start_identity",
            return_value=(17, "1234"),
        ):
            self.assertTrue(_same_process(expected))

    def test_empty_cmdline_exec_window_does_not_stale_remove_session(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-empty-cmdline-") as temp_dir:
            state = pathlib.Path(temp_dir) / "state.json"
            write_state(
                state,
                {
                    "argv_sha256": "a" * 64,
                    "pgid": 17,
                    "pid": 17,
                    "schema": "pipetune.worker-state/v1",
                    "session_id": "exec-window",
                    "start_ticks": "1234",
                    "workload_pgid": 17,
                    "workload_pid": 17,
                    "workload_start_ticks": "1234",
                },
            )
            with (
                mock.patch(
                    "pipetune.remote_worker._process_start_identity",
                    side_effect=[(17, "1234"), ProcessLookupError(17)],
                    create=True,
                ),
                mock.patch(
                    "pipetune.remote_worker._process_bytes",
                    side_effect=ProcessLookupError("empty cmdline during exec"),
                ),
                mock.patch("pipetune.remote_worker.os.killpg") as killpg,
            ):
                self.assertEqual(
                    terminate_session(state, "exec-window", grace_seconds=1.0),
                    "terminated",
                )
            killpg.assert_called_once_with(17, signal.SIGTERM)

    def test_exec_does_not_break_process_group_ownership(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-exec-") as temp_dir:
            root = pathlib.Path(temp_dir)
            state = root / "state.json"
            release = root / "release"
            child_code = (
                "import os, pathlib, sys, time\n"
                "release = pathlib.Path(sys.argv[1])\n"
                "while not release.exists():\n"
                "    time.sleep(0.01)\n"
                "os.execv(sys.executable, [sys.executable, '-c', "
                "'import time; time.sleep(30)'])\n"
            )
            launcher = subprocess.Popen(
                [
                    sys.executable,
                    str(WORKER),
                    "run",
                    "--state",
                    str(state),
                    "--session",
                    "exec-session",
                    "--cwd",
                    str(root),
                    "--stdout",
                    str(root / "stdout"),
                    "--stderr",
                    str(root / "stderr"),
                    "--",
                    sys.executable,
                    "-c",
                    child_code,
                    str(release),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            sentinel = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(30)"],
                start_new_session=True,
            )
            child_pid: int | None = None
            try:
                for _ in range(300):
                    if state.exists():
                        break
                    if launcher.poll() is not None:
                        self.fail(launcher.stderr.read().decode())
                    time.sleep(0.01)
                document = json.loads(state.read_text())
                child_pid = document["pid"]
                original_argv = document["argv_sha256"]
                release.touch()
                if not document["start_ticks"].startswith("portable-pgid:"):
                    for _ in range(300):
                        if process_identity(child_pid).argv_sha256 != original_argv:
                            break
                        time.sleep(0.01)
                    else:
                        self.fail("managed child did not exec")
                else:
                    time.sleep(0.1)
                self.assertEqual(
                    terminate_session(state, "exec-session", grace_seconds=1.0),
                    "terminated",
                )
                launcher.communicate(timeout=5)
                self.assertIsNone(sentinel.poll())
            finally:
                if child_pid is not None:
                    try:
                        os.killpg(child_pid, 9)
                    except ProcessLookupError:
                        pass
                if launcher.poll() is None:
                    launcher.kill()
                launcher.communicate()
                sentinel.terminate()
                sentinel.wait(timeout=5)

    def test_disconnect_and_interrupted_download_preserve_destination(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-disconnect-") as temp_dir:
            root = pathlib.Path(temp_dir)
            disconnect = root / "ssh-disconnect"
            write_executable(disconnect, "#!/bin/sh\nexit 255\n")
            transport = SshTransport(
                spec=ssh_endpoint(root),
                worker_path=str(WORKER),
                ssh_program=str(disconnect),
            )
            source = root / "source"
            source.write_bytes(b"payload")
            with self.assertRaises(TransportError):
                transport.upload(source, str(root / "remote"))

            partial = root / "ssh-partial"
            write_executable(
                partial,
                "#!/usr/bin/env python3\n"
                "import sys\n"
                "sys.stdout.buffer.write(b'partial')\n"
                "raise SystemExit(255)\n",
            )
            destination = root / "download"
            destination.write_bytes(b"original")
            partial_transport = SshTransport(
                spec=ssh_endpoint(root),
                worker_path=str(WORKER),
                ssh_program=str(partial),
            )
            with self.assertRaises(TransportError):
                partial_transport.download("remote", destination)
            self.assertEqual(destination.read_bytes(), b"original")
            self.assertEqual(list(root.glob(".download.*.tmp")), [])

    def test_worker_rejects_bad_state_and_cleanup_is_idempotent(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-state-") as temp_dir:
            root = pathlib.Path(temp_dir)
            state = root / "state.json"
            self.assertEqual(
                terminate_session(state, "session-a", grace_seconds=0.01),
                "already_stopped",
            )
            identity = process_identity(os.getpid())
            write_state(
                state,
                {
                    "schema": "pipetune.worker-state/v1",
                    "session_id": "session-a",
                    "pid": os.getpid(),
                    "pgid": os.getpid(),
                    "start_ticks": identity.start_ticks + "-stale",
                    "argv_sha256": identity.argv_sha256,
                    "workload_pid": os.getpid(),
                    "workload_pgid": os.getpid(),
                    "workload_start_ticks": identity.start_ticks,
                },
            )
            with self.assertRaises(WorkerStateError):
                terminate_session(state, "session-b", grace_seconds=0.01)
            with mock.patch("pipetune.remote_worker.os.killpg") as killpg:
                self.assertEqual(
                    terminate_session(state, "session-a", grace_seconds=0.01),
                    "stale_removed",
                )
                killpg.assert_not_called()
            self.assertFalse(state.exists())

    def test_worker_upload_and_sudo_failure_are_explicit(self) -> None:
        class InterruptedReader:
            def __init__(self) -> None:
                self.calls = 0

            def read(self, _size: int) -> bytes:
                self.calls += 1
                if self.calls == 1:
                    return b"partial"
                raise OSError("connection lost")

        with tempfile.TemporaryDirectory(prefix="pipetune-worker-") as temp_dir:
            root = pathlib.Path(temp_dir)
            destination = root / "destination"
            destination.write_bytes(b"original")
            expected = hashlib.sha256(b"complete").hexdigest()
            with self.assertRaises(OSError):
                receive_file(InterruptedReader(), destination, expected)
            self.assertEqual(destination.read_bytes(), b"original")
            self.assertEqual(list(root.glob(".destination.*.tmp")), [])

    def test_sudo_monitor_state_separates_launcher_and_workload_pid(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-sudo-monitor-") as temp_dir:
            root = pathlib.Path(temp_dir)
            fake_sudo = root / "sudo"
            write_executable(
                fake_sudo,
                "#!/usr/bin/env python3\n"
                "import os, sys\n"
                "arguments = sys.argv[1:]\n"
                "separator = arguments.index('--')\n"
                "command = arguments[separator + 1:]\n"
                "child = os.fork()\n"
                "if child == 0:\n"
                "    os.execvp(command[0], command)\n"
                "_, status = os.waitpid(child, 0)\n"
                "raise SystemExit(os.waitstatus_to_exitcode(status))\n",
            )
            state = root / "state.json"
            launcher = subprocess.Popen(
                [
                    sys.executable,
                    str(WORKER),
                    "run",
                    "--state",
                    str(state),
                    "--session",
                    "sudo-monitor",
                    "--cwd",
                    str(root),
                    "--stdout",
                    str(root / "stdout"),
                    "--stderr",
                    str(root / "stderr"),
                    "--sudo-program",
                    str(fake_sudo),
                    "--sudo",
                    "--",
                    sys.executable,
                    "-c",
                    "import time; time.sleep(30)",
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                for _ in range(300):
                    if state.exists():
                        break
                    if launcher.poll() is not None:
                        self.fail(launcher.stderr.read().decode())
                    time.sleep(0.01)
                else:
                    self.fail("sudo worker did not publish state")
                document = json.loads(state.read_text())
                self.assertNotEqual(document["pid"], document["workload_pid"])
                self.assertEqual(document["pgid"], document["workload_pgid"])
                self.assertEqual(
                    process_identity(document["workload_pid"]).start_ticks,
                    document["workload_start_ticks"],
                )
                self.assertEqual(
                    terminate_session(state, "sudo-monitor", grace_seconds=1.0),
                    "terminated",
                )
                launcher.communicate(timeout=5)
            finally:
                if launcher.poll() is None:
                    launcher.kill()
                launcher.communicate()

    def test_exec_workload_hands_readiness_back_to_worker_owner(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-ready-owner-") as temp_dir:
            ready = pathlib.Path(temp_dir) / "ready.json"
            completed = subprocess.run(
                [
                    sys.executable,
                    str(WORKER),
                    "exec-workload",
                    "--ready",
                    str(ready),
                    "--owner-uid",
                    str(os.getuid()),
                    "--owner-gid",
                    str(os.getgid()),
                    "--",
                    sys.executable,
                    "-c",
                    "pass",
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            document = json.loads(ready.read_text(encoding="utf-8"))
            self.assertEqual(document["schema"], "pipetune.workload-ready/v1")
            status = ready.stat()
            self.assertEqual((status.st_uid, status.st_gid), (os.getuid(), os.getgid()))
            self.assertEqual(status.st_mode & 0o777, 0o600)

    def test_exec_workload_rejects_invalid_handoff_owner(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-ready-owner-bad-") as temp_dir:
            ready = pathlib.Path(temp_dir) / "ready.json"
            for uid, gid in ((-1, os.getgid()), (os.getuid(), 2**32 - 1)):
                with self.subTest(uid=uid, gid=gid):
                    with self.assertRaises(WorkerStateError):
                        exec_workload(
                            ready,
                            (sys.executable, "-c", "pass"),
                            owner_uid=uid,
                            owner_gid=gid,
                        )
                    self.assertFalse(ready.exists())

    def test_transport_cleanup_is_contained_and_idempotent(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-cleanup-") as temp_dir:
            root = pathlib.Path(temp_dir)
            containment = root / "trials"
            trial = containment / "trial-1"
            trial.mkdir(parents=True)
            (trial / "artifact").write_bytes(b"value")
            transport = LocalTransport(worker_path=WORKER)
            transport.remove_tree(str(trial), containment_root=str(containment))
            self.assertFalse(trial.exists())
            transport.remove_tree(str(trial), containment_root=str(containment))
            transport.remove_tree(
                str(root / "never-created" / "trial-2"),
                containment_root=str(root / "never-created"),
            )
            with self.assertRaises(TransportError):
                transport.remove_tree(str(containment), containment_root=str(containment))
            outside = root / "outside"
            outside.mkdir()
            with self.assertRaises(TransportError):
                transport.remove_tree(str(outside), containment_root=str(containment))

            sudo = root / "sudo"
            argv_log = root / "sudo.argv.json"
            write_executable(
                sudo,
                "#!/usr/bin/env python3\n"
                "import json, pathlib, sys\n"
                f"pathlib.Path({str(argv_log)!r}).write_text(json.dumps(sys.argv[1:]))\n"
                "raise SystemExit(23)\n",
            )
            result = run_foreground(
                state_path=root / "sudo-state.json",
                session_id="sudo-failure",
                cwd=root,
                stdout_path=root / "sudo.stdout",
                stderr_path=root / "sudo.stderr",
                argv=(sys.executable, "-c", "print('must not run')"),
                use_sudo=True,
                sudo_program=str(sudo),
            )
            self.assertEqual(result["return_code"], 23)
            self.assertEqual(json.loads(argv_log.read_text())[:2], ["-n", "--"])
            self.assertFalse((root / "sudo-state.json").exists())

    def test_resolver_rejects_invalid_dump(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-resolve-bad-") as temp_dir:
            root = pathlib.Path(temp_dir)
            configure = root / "configure"
            write_executable(configure, "#!/bin/sh\nprintf '{bad json}'\n")
            config = root / "config.toml"
            config.write_text("unused\n")
            with self.assertRaises((ContractError, TransportError)):
                resolve_endpoint(
                    endpoint_id="target",
                    config_path=config,
                    configure_binary=configure,
                )


if __name__ == "__main__":
    unittest.main()
