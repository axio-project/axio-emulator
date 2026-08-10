"""Remote preflight and build-cache operations shared by all AE experiments."""

from __future__ import annotations

import dataclasses
import hashlib
import json
import pathlib
import re
import time
import uuid
from collections.abc import Callable, Sequence

from pipetune.artifacts import write_json_atomic
from pipetune.remote import (
    EndpointTransport,
    ResolvedEndpoint,
    TransportError,
    transport_for,
)
from pipetune.runner import AxioConfigTool


class ArtifactRuntimeError(RuntimeError):
    pass


def _single_rdma_netdev(output: bytes) -> str:
    netdevs = [line.strip() for line in output.decode().splitlines() if line.strip()]
    if len(netdevs) != 1:
        raise ArtifactRuntimeError(
            "configured RDMA device must expose exactly one Linux netdev"
        )
    return netdevs[0]


@dataclasses.dataclass(frozen=True)
class CommandEvidence:
    label: str
    argv: tuple[str, ...]
    stdout: pathlib.Path
    stderr: pathlib.Path


class EndpointCommands:
    def __init__(
        self,
        endpoint: ResolvedEndpoint,
        transport: EndpointTransport,
        evidence_root: pathlib.Path,
    ) -> None:
        self.endpoint = endpoint
        self.transport = transport
        self.evidence_root = evidence_root

    def capture(
        self,
        label: str,
        argv: Sequence[str],
        *,
        timeout_seconds: float = 60.0,
        use_sudo: bool = False,
    ) -> bytes:
        token = uuid.uuid4().hex
        remote_root = pathlib.PurePosixPath(self.endpoint.spec.workdir) / ".artifact-eval/control"
        state = str(remote_root / f"{token}.state.json")
        stdout = str(remote_root / f"{token}.stdout")
        stderr = str(remote_root / f"{token}.stderr")
        outcome = self.transport.run(
            session_id=f"ae-{token}",
            argv=tuple(argv),
            cwd=self.endpoint.spec.workdir,
            state_path=state,
            stdout_path=stdout,
            stderr_path=stderr,
            timeout_seconds=timeout_seconds,
            use_sudo=use_sudo,
        )
        try:
            stdout_bytes = self.transport.get_bytes(stdout)
            stderr_bytes = self.transport.get_bytes(stderr)
        except TransportError as error:
            raise ArtifactRuntimeError(f"{label}: cannot retrieve command evidence: {error}") from error
        local_stdout = self.evidence_root / f"{label}.stdout"
        local_stderr = self.evidence_root / f"{label}.stderr"
        local_stdout.parent.mkdir(parents=True, exist_ok=True)
        local_stdout.write_bytes(stdout_bytes)
        local_stderr.write_bytes(stderr_bytes)
        if outcome.status != "exited" or outcome.return_code != 0:
            reason = stderr_bytes.decode(errors="replace").strip()
            fallback = outcome.failure_reason or f"exit code {outcome.return_code}"
            raise ArtifactRuntimeError(f"{label} failed: {reason or fallback}")
        return stdout_bytes


@dataclasses.dataclass(frozen=True)
class BuildRecord:
    endpoint_id: str
    role: str
    build_fingerprint: str
    source_config_sha256: str
    binary_path: str
    binary_sha256: str
    build_directory: str

    def as_document(self) -> dict[str, object]:
        return dataclasses.asdict(self)


class BuildCache:
    def __init__(
        self,
        *,
        configure_binary: pathlib.Path,
        evidence_root: pathlib.Path,
        expected_git_commit: str,
        transport_factory: Callable[[object], EndpointTransport] = transport_for,
    ) -> None:
        self._tool = AxioConfigTool(configure_binary)
        self._evidence_root = evidence_root
        self._expected_git_commit = expected_git_commit
        self._transport_factory = transport_factory
        self._records: dict[tuple[str, str], BuildRecord] = {}

    @staticmethod
    def _safe_fingerprint(value: str) -> str:
        result = re.sub(r"[^A-Za-z0-9._-]", "-", value)
        if not result:
            raise ArtifactRuntimeError("empty build fingerprint")
        return result

    def ensure(self, endpoint_id: str, config_path: pathlib.Path) -> BuildRecord:
        fingerprints = self._tool.fingerprints(config_path)
        build_fingerprint = fingerprints["build"]
        resolved = self._tool.resolve(
            endpoint_id=endpoint_id,
            config_path=config_path,
            binary_override=None,
        )
        cache_key = (endpoint_id, build_fingerprint)
        if cache_key in self._records:
            return self._records[cache_key]
        transport = self._transport_factory(resolved.spec)
        commands = EndpointCommands(
            resolved,
            transport,
            self._evidence_root / endpoint_id / self._safe_fingerprint(build_fingerprint),
        )
        git_commit = commands.capture("git-commit", ("git", "rev-parse", "HEAD"))
        if git_commit.decode().strip() != self._expected_git_commit:
            raise ArtifactRuntimeError(f"{endpoint_id}: remote Git SHA does not match controller")

        config_payload = config_path.read_bytes()
        config_sha = hashlib.sha256(config_payload).hexdigest()
        remote_config = str(
            pathlib.PurePosixPath(resolved.spec.workdir)
            / ".artifact-eval/build-configs"
            / f"{self._safe_fingerprint(build_fingerprint)}.toml"
        )
        transport.put_bytes(remote_config, config_payload)
        build_dir = str(
            pathlib.PurePosixPath(resolved.spec.workdir)
            / "build-ae"
            / self._safe_fingerprint(build_fingerprint)
        )
        commands.capture(
            "meson-setup",
            ("meson", "setup", build_dir, f"-Daxio_config={remote_config}"),
            timeout_seconds=300.0,
        )
        commands.capture(
            "build",
            ("python3", "toolchain/axio_build.py", build_dir, "--target", "axio"),
            timeout_seconds=1200.0,
        )
        binary_path = str(pathlib.PurePosixPath(build_dir) / "axio")
        sha_output = commands.capture(
            "binary-sha256", ("sha256sum", binary_path), timeout_seconds=30.0
        ).decode()
        binary_sha = sha_output.split()[0] if sha_output.split() else ""
        if not re.fullmatch(r"[0-9a-f]{64}", binary_sha):
            raise ArtifactRuntimeError(f"{endpoint_id}: invalid binary SHA-256 output")
        record = BuildRecord(
            endpoint_id=endpoint_id,
            role=resolved.spec.role,
            build_fingerprint=build_fingerprint,
            source_config_sha256=config_sha,
            binary_path=binary_path,
            binary_sha256=binary_sha,
            build_directory=build_dir,
        )
        self._records[cache_key] = record
        return record


class Preflight:
    def __init__(
        self,
        *,
        configure_binary: pathlib.Path,
        evidence_root: pathlib.Path,
        expected_git_commit: str,
        transport_factory: Callable[[object], EndpointTransport] = transport_for,
    ) -> None:
        self._tool = AxioConfigTool(configure_binary)
        self._evidence_root = evidence_root
        self._git_commit = expected_git_commit
        self._transport_factory = transport_factory

    def check_pair(
        self, target_config: pathlib.Path, peer_config: pathlib.Path
    ) -> dict[str, object]:
        self._tool.validate_pair(target_config, peer_config)
        result: dict[str, object] = {}
        roce_endpoints: dict[str, tuple[ResolvedEndpoint, EndpointTransport, dict[str, object]]] = {}
        for endpoint_id, config in (("target", target_config), ("peer", peer_config)):
            document = self._tool.dump(config)
            resolved = self._tool.resolve(
                endpoint_id=endpoint_id, config_path=config, binary_override=None
            )
            commands = EndpointCommands(
                resolved,
                self._transport_factory(resolved.spec),
                self._evidence_root / endpoint_id,
            )
            deployment = document["deployment"]
            network = document["network"]
            topology = deployment["topology"]
            workspace_count = len(topology["workspaces"])
            if deployment["numa_node"] != 1 or workspace_count < 16:
                raise ArtifactRuntimeError(
                    f"{endpoint_id}: reference profile requires 16 workspaces on NUMA 1"
                )
            git_commit = commands.capture("git-commit", ("git", "rev-parse", "HEAD")).decode().strip()
            if git_commit != self._git_commit:
                raise ArtifactRuntimeError(f"{endpoint_id}: remote Git SHA mismatch")
            commands.capture("sudo", ("sudo", "-n", "true"))
            versions = {}
            for label, argv, use_sudo in (
                ("python", ("python3", "--version"), False),
                ("meson", ("meson", "--version"), False),
                ("ninja", ("ninja", "--version"), False),
                ("perf", ("/usr/bin/perf", "--version"), False),
                (
                    "pcm-pcie",
                    ("env", "LC_ALL=C", "dpkg-query", "-W", "pcm"),
                    False,
                ),
            ):
                versions[label] = commands.capture(
                    label, argv, use_sudo=use_sudo
                ).decode(errors="replace").strip()
            commands.capture(
                "pcm-msr-access",
                ("test", "-r", "/dev/cpu/0/msr"),
                use_sudo=True,
            )
            lscpu = commands.capture("numa-cpus", ("lscpu", "-p=CPU,NODE")).decode()
            cpus = [
                line for line in lscpu.splitlines()
                if line and not line.startswith("#") and line.rsplit(",", 1)[-1] == "1"
            ]
            if len(cpus) < 16:
                raise ArtifactRuntimeError(f"{endpoint_id}: NUMA 1 exposes fewer than 16 CPUs")
            device = str(network["device_name"])
            bdf = str(network["device_pcie"])
            commands.capture(
                "pci-device", ("test", "-e", f"/sys/bus/pci/devices/{bdf}")
            )
            pci_numa = commands.capture(
                "pci-numa", ("cat", f"/sys/bus/pci/devices/{bdf}/numa_node")
            ).decode().strip()
            if pci_numa != "1":
                raise ArtifactRuntimeError(f"{endpoint_id}: configured PCI device is not on NUMA 1")
            free_hugepages = commands.capture(
                "hugepages",
                (
                    "cat",
                    "/sys/devices/system/node/node1/hugepages/hugepages-2048kB/free_hugepages",
                ),
            ).decode().strip()
            if not free_hugepages.isdigit() or int(free_hugepages) <= 0:
                raise ArtifactRuntimeError(f"{endpoint_id}: NUMA 1 has no free 2 MiB hugepages")
            if network["backend"] == "roce":
                commands.capture(
                    "nic-device", ("test", "-e", f"/sys/class/infiniband/{device}")
                )
                netdev = _single_rdma_netdev(
                    commands.capture(
                        "nic-netdev",
                        ("ls", "-1", f"/sys/class/infiniband/{device}/device/net"),
                    )
                )
                ip_document = json.loads(
                    commands.capture(
                        "network-address",
                        ("ip", "-j", "address", "show", "dev", netdev),
                    )
                )
                if not isinstance(ip_document, list) or len(ip_document) != 1:
                    raise ArtifactRuntimeError(
                        f"{endpoint_id}: cannot inspect configured NIC"
                    )
                address = ip_document[0]
                ips = {
                    item.get("local")
                    for item in address.get("addr_info", [])
                    if isinstance(item, dict)
                }
                if (
                    address.get("address") != network["local_mac"]
                    or network["local_ip"] not in ips
                ):
                    raise ArtifactRuntimeError(
                        f"{endpoint_id}: configured IP/MAC does not match NIC"
                    )
                commands.capture("roce-device", ("ibv_devinfo", "-d", device, "-i", "1"))
                commands.capture("roce-ping", ("ping", "-c", "1", str(network["remote_ip"])))
                roce_endpoints[resolved.spec.role] = (
                    resolved,
                    commands.transport,
                    network,
                )
            else:
                commands.capture(
                    "dpdk-driver",
                    ("readlink", "-f", f"/sys/bus/pci/devices/{bdf}/driver"),
                )
            result[endpoint_id] = {
                "git_commit": git_commit,
                "numa_node": 1,
                "numa_cpu_count": len(cpus),
                "workspace_pool": workspace_count,
                "device_name": device,
                "netdev_name": netdev if network["backend"] == "roce" else None,
                "device_pcie": bdf,
                "backend": network["backend"],
                "versions": versions,
            }
        if roce_endpoints:
            result["roce_link"] = self._check_roce_bandwidth(roce_endpoints)
        return result

    def _check_roce_bandwidth(
        self,
        endpoints: dict[str, tuple[ResolvedEndpoint, EndpointTransport, dict[str, object]]],
    ) -> dict[str, object]:
        if set(endpoints) != {"client", "server"}:
            raise ArtifactRuntimeError("RoCE preflight requires client and server endpoints")
        server, server_transport, server_network = endpoints["server"]
        client, client_transport, client_network = endpoints["client"]
        token = uuid.uuid4().hex
        remote_root = pathlib.PurePosixPath(server.spec.workdir) / ".artifact-eval/control"
        prefix = remote_root / f"ib-write-bw-{token}"
        common = (
            "ib_write_bw",
            "-d",
            str(server_network["device_name"]),
            "-i",
            "1",
            "-x",
            "3",
            "-F",
            "--report_gbits",
            "-s",
            "65536",
            "-n",
            "100",
        )
        handle = server_transport.start(
            session_id=f"ae-ib-write-bw-{token}",
            argv=common,
            cwd=server.spec.workdir,
            state_path=str(prefix.with_suffix(".state.json")),
            stdout_path=str(prefix.with_suffix(".stdout")),
            stderr_path=str(prefix.with_suffix(".stderr")),
        )
        try:
            time.sleep(0.5)
            client_commands = EndpointCommands(
                client,
                client_transport,
                self._evidence_root / "client",
            )
            client_output = client_commands.capture(
                "ib-write-bw",
                (
                    "ib_write_bw",
                    "-d",
                    str(client_network["device_name"]),
                    "-i",
                    "1",
                    "-x",
                    "3",
                    "-F",
                    "--report_gbits",
                    "-s",
                    "65536",
                    "-n",
                    "100",
                    str(client_network["remote_ip"]),
                ),
                timeout_seconds=30.0,
            )
            outcome = server_transport.wait(handle, timeout_seconds=30.0)
        except BaseException:
            server_transport.terminate(handle)
            raise
        server_stdout = server_transport.get_bytes(str(prefix.with_suffix(".stdout")))
        server_stderr = server_transport.get_bytes(str(prefix.with_suffix(".stderr")))
        server_root = self._evidence_root / "server"
        server_root.mkdir(parents=True, exist_ok=True)
        (server_root / "ib-write-bw.stdout").write_bytes(server_stdout)
        (server_root / "ib-write-bw.stderr").write_bytes(server_stderr)
        if outcome.status != "exited" or outcome.return_code != 0:
            raise ArtifactRuntimeError("RoCE ib_write_bw server failed")
        if b"BW average" not in client_output:
            raise ArtifactRuntimeError("RoCE ib_write_bw output is incomplete")
        return {"iterations": 100, "message_bytes": 65536, "status": "complete"}


def write_build_records(path: pathlib.Path, records: Sequence[BuildRecord]) -> None:
    write_json_atomic(path, {"builds": [record.as_document() for record in records]})
