from __future__ import annotations

import copy
import dataclasses
import json
import pathlib
import tempfile
import typing
import unittest

from pipetune.artifacts import (
    load_metric_sample,
    load_session_manifest,
    load_trial_manifest,
)
from pipetune.model import EndpointSpec
from pipetune.remote import CommandOutcome, ResolvedEndpoint, TransportError
from pipetune import runner as runner_module
from pipetune.runner import AxioConfigTool, MeasureError, MeasureRequest, measure


ROOT = pathlib.Path(__file__).resolve().parents[2]
VALID_METRICS = (ROOT / "tests/metrics/fixtures/valid-window.jsonl").read_bytes()
VALID_STAGE_DISTRIBUTION = (
    b'{"schema":"axio.stage-distribution/v1","window_id":0,'
    b'"sample_stride":64,"stages":{'
    b'"app_tx_allocation_stall":{"unit":"us_per_batch","p1_us":0.04,'
    b'"p50_us":0.05,"p99_us":0.10,"mean_us":0.06,"min_us":0.03,'
    b'"max_us":0.20,"sample_count":100},'
    b'"app_rx_handler_completion":{"unit":"us_per_batch","p1_us":0.08,'
    b'"p50_us":0.10,"p99_us":0.15,"mean_us":0.11,"min_us":0.07,'
    b'"max_us":0.30,"sample_count":100}}}\n'
)
GIT_SHA = "b" * 40
BINARY_SHA = "a" * 64
START = "2026-08-08T00:00:00+00:00"
END = "2026-08-08T00:00:01+00:00"


def metrics_windows(*window_ids: int) -> bytes:
    template = json.loads(VALID_METRICS)
    documents = []
    for window_id in window_ids:
        document = copy.deepcopy(template)
        document["window_id"] = window_id
        documents.append(json.dumps(document, separators=(",", ":")) + "\n")
    return "".join(documents).encode()


def config_document(role: str) -> dict[str, object]:
    return {
        "deployment": {
            "host": "",
            "numa_node": 1,
            "role": role,
            "ssh_port": 0,
            "ssh_user": "",
            "transport": "local",
            "use_sudo": True,
            "workdir": "/opt/axio",
        },
        "knobs": {
            "runtime": {
                "application_core_count": 2,
                "dispatcher_queue_count": 2,
            },
        },
        "metrics": {
            "enabled": True,
            "human_output": True,
            "jsonl_path": "results/source.jsonl",
            "stage_distribution": {
                "enabled": False,
                "jsonl_path": "results/stage-distribution.jsonl",
                "sample_capacity": 65536,
                "sample_stride": 64,
            },
        },
        "network": {"backend": "dpdk"},
        "other": {"iterations": 1, "window_seconds": 1},
        "tuning": {"sample_windows": 1, "warmup_windows": 0},
    }


class FakeConfigTool:
    def __init__(self, target_role: str) -> None:
        peer_role = "server" if target_role == "client" else "client"
        self.documents = {
            "target.toml": config_document(target_role),
            "peer.toml": config_document(peer_role),
        }
        self.materializations: list[tuple[str, str]] = []

    def resolve(
        self,
        *,
        endpoint_id: str,
        config_path: pathlib.Path,
        binary_override: str | None,
    ) -> ResolvedEndpoint:
        document = self.dump(config_path)
        deployment = document["deployment"]
        spec = EndpointSpec(
            endpoint_id=endpoint_id,
            role=deployment["role"],
            backend=document["network"]["backend"],
            transport=deployment["transport"],
            host=deployment["host"],
            ssh_port=deployment["ssh_port"],
            ssh_user=deployment["ssh_user"],
            workdir=deployment["workdir"],
            use_sudo=deployment["use_sudo"],
            numa_node=deployment["numa_node"],
        )
        binary = binary_override or f"/opt/axio/build-{spec.role}/axio"
        return ResolvedEndpoint(spec, config_path.resolve(), binary)

    def dump(self, path: pathlib.Path) -> dict[str, object]:
        if path.name in self.documents:
            return copy.deepcopy(self.documents[path.name])
        return json.loads(path.read_text(encoding="utf-8"))

    def materialize_runner_pair(
        self,
        *,
        target_input: pathlib.Path,
        peer_input: pathlib.Path,
        target_output: pathlib.Path,
        peer_output: pathlib.Path,
        target_metrics_path: str,
        peer_metrics_path: str,
    ) -> None:
        for source, output, metrics_path in (
            (target_input, target_output, target_metrics_path),
            (peer_input, peer_output, peer_metrics_path),
        ):
            document = self.dump(source)
            before = copy.deepcopy(document)
            document["metrics"]["jsonl_path"] = metrics_path
            document["metrics"]["human_output"] = False
            distribution = document["metrics"]["stage_distribution"]
            if distribution["enabled"]:
                distribution["jsonl_path"] = str(
                    pathlib.PurePosixPath(metrics_path).with_name(
                        "stage-distribution.jsonl"
                    )
                )
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps(document), encoding="utf-8")
            self.materializations.append((source.name, metrics_path))
            self.assert_runner_only_changes(before, document)

    def fingerprints(self, path: pathlib.Path) -> dict[str, str]:
        del path
        return {
            "build": "fnv1a64:0000000000000001",
            "datapath": "fnv1a64:0000000000000002",
            "deployment": "fnv1a64:0000000000000003",
        }

    @staticmethod
    def assert_runner_only_changes(
        source: dict[str, object], materialized: dict[str, object]
    ) -> None:
        expected = copy.deepcopy(source)
        expected["metrics"]["jsonl_path"] = materialized["metrics"]["jsonl_path"]
        expected["metrics"]["human_output"] = False
        distribution = expected["metrics"]["stage_distribution"]
        if distribution["enabled"]:
            distribution["jsonl_path"] = materialized["metrics"][
                "stage_distribution"
            ]["jsonl_path"]
        if materialized != expected:
            raise MeasureError("runner changed a non-artifact config field")


class FakeHandle:
    def __init__(self, transport: "ScriptedTransport", argv: tuple[str, ...]) -> None:
        self.transport = transport
        self.argv = argv


class ScriptedTransport:
    def __init__(
        self,
        spec: EndpointSpec,
        events: list[str],
        *,
        start_failure: bool = False,
        endpoint_return_code: int = 0,
        provider_permission_failure: bool = False,
        pcm_permission_failure: bool = False,
        pcm_timeout_with_data: bool = False,
        provider_retrieval_failure: bool = False,
        retrieval_failure: bool = False,
        upload_failure: bool = False,
        metrics_snapshots: tuple[bytes | None, ...] = (VALID_METRICS,),
        running_checks_before_exit: int | None = None,
    ) -> None:
        self.spec = spec
        self.events = events
        self.start_failure = start_failure
        self.endpoint_return_code = endpoint_return_code
        self.provider_permission_failure = provider_permission_failure
        self.pcm_permission_failure = pcm_permission_failure
        self.pcm_timeout_with_data = pcm_timeout_with_data
        self.provider_retrieval_failure = provider_retrieval_failure
        self.retrieval_failure = retrieval_failure
        self.upload_failure = upload_failure
        self.metrics_snapshots = metrics_snapshots
        self.running_checks_before_exit = running_checks_before_exit
        self.files: dict[str, bytes] = {}
        self.metrics_path = ""
        self.stage_distribution_path = ""
        self.provider_runs = 0
        self.perf_pids: list[int] = []
        self.terminated = 0
        self.cleaned = 0
        self.metrics_reads = 0
        self.running_checks = 0

    def put_bytes(self, destination: str, payload: bytes) -> None:
        if self.upload_failure:
            raise TransportError("scripted upload failure")
        self.files[destination] = payload
        if destination.endswith(".toml"):
            document = json.loads(payload)
            self.metrics_path = document["metrics"]["jsonl_path"]
            distribution = document["metrics"]["stage_distribution"]
            if distribution["enabled"]:
                self.stage_distribution_path = distribution["jsonl_path"]

    def get_bytes(self, source: str) -> bytes:
        if self.provider_retrieval_failure and source.endswith("pcm-pcie.csv"):
            raise TransportError("scripted provider retrieval failure")
        if self.retrieval_failure and source == self.metrics_path:
            raise TransportError("scripted retrieval failure")
        if source == self.metrics_path:
            index = min(self.metrics_reads, len(self.metrics_snapshots) - 1)
            self.metrics_reads += 1
            snapshot = self.metrics_snapshots[index]
            if snapshot is None:
                raise TransportError("metrics file is not ready")
            self.events.append(f"metrics:{self.spec.endpoint_id}:{index}")
            return snapshot
        if source == self.stage_distribution_path:
            return VALID_STAGE_DISTRIBUTION
        try:
            return self.files[source]
        except KeyError as error:
            raise TransportError(f"missing scripted artifact {source}") from error

    def start(self, **kwargs: object) -> FakeHandle:
        if self.start_failure:
            raise TransportError("scripted readiness timeout")
        argv = tuple(kwargs["argv"])
        self.events.append(f"start:{self.spec.role}:{self.spec.endpoint_id}")
        self.files[str(kwargs["state_path"])] = json.dumps(
            {
                "pid": 1000 + (1 if self.spec.role == "client" else 2),
                "workload_pid": 2000 + (1 if self.spec.role == "client" else 2),
            }
        ).encode()
        self.files[str(kwargs["stdout_path"])] = b"axio stdout\n"
        self.files[str(kwargs["stderr_path"])] = b""
        return FakeHandle(self, argv)

    def wait(
        self, handle: FakeHandle, *, timeout_seconds: float | None = None
    ) -> CommandOutcome:
        del timeout_seconds
        return CommandOutcome(
            argv=handle.argv,
            status="exited",
            return_code=self.endpoint_return_code,
            failure_reason=None,
            started_at_utc=START,
            ended_at_utc=END,
        )

    def is_running(self, handle: FakeHandle) -> bool:
        del handle
        self.running_checks += 1
        if self.running_checks_before_exit is None:
            return True
        return self.running_checks <= self.running_checks_before_exit

    def terminate(self, handle: FakeHandle) -> CommandOutcome:
        self.terminated += 1
        return self.wait(handle)

    def run(self, **kwargs: object) -> CommandOutcome:
        argv = tuple(kwargs["argv"])
        stdout_path = str(kwargs["stdout_path"])
        stderr_path = str(kwargs["stderr_path"])
        stdout = b""
        stderr = b""
        status = "exited"
        return_code = 0
        failure_reason = None
        if argv[:3] == ("git", "-C", "/opt/axio"):
            stdout = (GIT_SHA + "\n").encode()
        elif argv and argv[0] == "sha256sum":
            stdout = f"{BINARY_SHA}  {argv[1]}\n".encode()
        elif argv[-1:] == ("--version",):
            self.events.append(f"probe:perf:{self.spec.endpoint_id}")
            stdout = b"perf version 5.15.160\n"
        elif "dpkg-query" in argv:
            self.events.append(f"probe:pcm:{self.spec.endpoint_id}")
            stdout = b"pcm\t202201-1\n"
        elif "/usr/bin/perf" in argv:
            self.provider_runs += 1
            self.events.append(f"collect:perf:{self.spec.endpoint_id}")
            self.perf_pids.append(int(argv[argv.index("-p") + 1]))
            if self.provider_permission_failure:
                return_code = 1
                stderr = b"No permission to enable LLC event\n"
            elif "LLC-loads,LLC-load-misses" in argv:
                stderr = (
                    b"1.0;1000;;LLC-loads;1000000000;100.00;;\n"
                    b"1.0;100;;LLC-load-misses;1000000000;100.00;10.0;ratio\n"
                )
            else:
                stderr = (
                    b"1.0;500;;LLC-stores;1000000000;100.00;;\n"
                    b"1.0;20;;LLC-store-misses;1000000000;100.00;4.0;ratio\n"
                )
        elif "/usr/sbin/pcm-pcie" in argv:
            self.provider_runs += 1
            output = next(value.split("=", 1)[1] for value in argv if value.startswith("-csv="))
            if self.pcm_permission_failure:
                return_code = 1
                stderr = b"PCM Error: can't open MSR handle for core 0\n"
            else:
                self.files[output] = (
                    b"Skt,PCIRdCur,ItoM,Status\n"
                    b"1,100,200,Total\n1,10,20,Miss\n1,90,180,Hit\n"
                )
                if self.pcm_timeout_with_data:
                    status = "timed_out"
                    return_code = None
                    failure_reason = "command exceeded its timeout"
        self.files[stdout_path] = stdout
        self.files[stderr_path] = stderr
        return CommandOutcome(
            argv=argv,
            status=status,
            return_code=return_code,
            failure_reason=failure_reason,
            started_at_utc=START,
            ended_at_utc=END,
        )

    def remove_tree(self, path: str, *, containment_root: str) -> None:
        self.cleaned += 1
        prefix = path.rstrip("/") + "/"
        self.files = {
            key: value for key, value in self.files.items() if not key.startswith(prefix)
        }
        self.events.append(f"cleanup:{self.spec.endpoint_id}")


class RunnerTest(unittest.TestCase):
    def test_runner_type_annotations_resolve_for_reflection(self) -> None:
        for function in (runner_module._remote_layout, runner_module.measure):
            with self.subTest(function=function.__name__):
                hints = typing.get_type_hints(function)
                self.assertIn("return", hints)

    def request(self, root: pathlib.Path) -> MeasureRequest:
        target = root / "target.toml"
        peer = root / "peer.toml"
        target.write_text("target")
        peer.write_text("peer")
        return MeasureRequest(
            target_config=target,
            peer_config=peer,
            output=root / "result",
            configure_binary=root / "axio-configure",
        )

    def test_request_rejects_non_finite_timeouts(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-request-") as temp_dir:
            root = pathlib.Path(temp_dir)
            request = self.request(root)
            for field, value in (
                ("ready_timeout_seconds", float("nan")),
                ("ready_timeout_seconds", float("inf")),
                ("completion_grace_seconds", float("-inf")),
            ):
                with self.subTest(field=field, value=value), self.assertRaises(
                    MeasureError
                ):
                    dataclasses.replace(request, **{field: value})

    def run_measure(
        self,
        root: pathlib.Path,
        *,
        target_role: str = "client",
        target_options: dict[str, object] | None = None,
        peer_options: dict[str, object] | None = None,
        warmup_windows: int = 0,
        sample_windows: int = 1,
        ready_timeout_seconds: float = 10.0,
        stage_distribution: bool = False,
        progress: object = None,
    ):
        tool = FakeConfigTool(target_role)
        for document in tool.documents.values():
            document["tuning"]["warmup_windows"] = warmup_windows
            document["tuning"]["sample_windows"] = sample_windows
            document["other"]["iterations"] = warmup_windows + sample_windows
            document["metrics"]["stage_distribution"]["enabled"] = stage_distribution
        target = tool.resolve(
            endpoint_id="target",
            config_path=root / "target.toml",
            binary_override=None,
        )
        peer = tool.resolve(
            endpoint_id="peer",
            config_path=root / "peer.toml",
            binary_override=None,
        )
        events: list[str] = []
        transports = {
            "target": ScriptedTransport(target.spec, events, **(target_options or {})),
            "peer": ScriptedTransport(peer.spec, events, **(peer_options or {})),
        }
        self.last_transports = transports
        self.last_events = events
        result = measure(
            dataclasses.replace(
                self.request(root), ready_timeout_seconds=ready_timeout_seconds
            ),
            config_tool=tool,
            transport_factory=lambda spec: transports[spec.endpoint_id],
            sleeper=lambda _seconds: None,
            trial_id_factory=lambda: "trial-0001",
            progress=progress,
        )
        return result, tool, transports, events

    def test_measure_reports_each_blocking_stage_when_requested(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-runner-") as temp_dir:
            messages: list[str] = []

            self.run_measure(
                pathlib.Path(temp_dir),
                warmup_windows=0,
                sample_windows=1,
                progress=messages.append,
            )

        self.assertEqual(
            messages,
            [
                "Preparing trial trial-0001",
                "Trial trial-0001: DPDK target=client, peer=server, "
                "warmup=0 windows, sample=1 windows",
                "Trial trial-0001: starting server endpoint peer",
                "Trial trial-0001: starting client endpoint target",
                "Trial trial-0001: waiting for 0 warmup windows",
                "Trial trial-0001: collecting 1 sample windows and host counters",
                "Trial trial-0001: finalizing artifacts",
                "Trial trial-0001: complete",
            ],
        )

    def test_target_client_trial_is_role_ordered_target_only_and_valid(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-runner-") as temp_dir:
            root = pathlib.Path(temp_dir)
            result, tool, transports, events = self.run_measure(root)
            self.assertTrue(result.success)
            starts = [event for event in events if event.startswith("start:")]
            self.assertEqual(starts, ["start:server:peer", "start:client:target"])
            self.assertLess(events.index("probe:perf:target"), events.index(starts[0]))
            self.assertLess(events.index("probe:pcm:target"), events.index(starts[0]))
            self.assertGreater(transports["target"].provider_runs, 0)
            self.assertEqual(transports["peer"].provider_runs, 0)
            self.assertEqual(transports["target"].perf_pids, [2001, 2001])
            self.assertEqual(tool.materializations, [
                ("target.toml", "/opt/axio/.pipetune/trials/trial-0001/target/metrics.jsonl"),
                ("peer.toml", "/opt/axio/.pipetune/trials/trial-0001/peer/metrics.jsonl"),
            ])
            session = load_session_manifest(
                root / "result" / "session.json", artifact_root=root / "result"
            )
            self.assertEqual(session.status, "complete")
            self.assertEqual(len(session.trials), 1)
            manifest_path = root / "result" / session.trials[0].path
            manifest = load_trial_manifest(
                manifest_path, artifact_root=manifest_path.parent
            )
            self.assertEqual(manifest.target_endpoint_id, "target")
            self.assertEqual([endpoint.spec.role for endpoint in manifest.endpoints], [
                "client", "server"
            ])
            target_endpoint = next(
                endpoint
                for endpoint in manifest.endpoints
                if endpoint.spec.endpoint_id == "target"
            )
            canonical = next(
                artifact
                for artifact in target_endpoint.artifacts
                if artifact.path == "configs/canonical/target.json"
            )
            canonical_document = json.loads(
                (manifest_path.parent / canonical.path).read_text()
            )
            self.assertEqual(canonical_document["tuning"]["sample_windows"], 1)
            sample = load_metric_sample(
                manifest_path.parent / manifest.host_metrics.path,
                artifact_root=manifest_path.parent,
            )
            self.assertTrue(all(counter.available for counter in sample.counters))
            self.assertEqual(len(sample.commands), 5)
            self.assertTrue(
                all(command.status == "exited" for command in sample.commands)
            )
            self.assertEqual(transports["target"].cleaned, 1)
            self.assertEqual(transports["peer"].cleaned, 1)
            self.assertEqual(list(root.glob(".result.*.tmp")), [])

    def test_enabled_stage_distribution_is_preserved_as_endpoint_artifact(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-runner-") as temp_dir:
            root = pathlib.Path(temp_dir)
            self.run_measure(root, stage_distribution=True)
            trial_root = root / "result/trials/trial-0001"
            manifest = load_trial_manifest(
                trial_root / "trial.json", artifact_root=trial_root
            )
            for endpoint in manifest.endpoints:
                paths = {artifact.path for artifact in endpoint.artifacts}
                path = f"endpoints/{endpoint.spec.endpoint_id}/stage-distribution.jsonl"
                self.assertIn(path, paths)
                self.assertEqual(
                    (trial_root / path).read_bytes(), VALID_STAGE_DISTRIBUTION
                )

    def test_collectors_start_after_both_endpoints_publish_valid_warmup(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-runner-") as temp_dir:
            root = pathlib.Path(temp_dir)
            _result, _tool, transports, events = self.run_measure(
                root,
                warmup_windows=2,
                target_options={
                    "metrics_snapshots": (
                        None,
                        metrics_windows(0),
                        metrics_windows(0, 1),
                        metrics_windows(0, 1, 2),
                    ),
                },
                peer_options={
                    "metrics_snapshots": (
                        metrics_windows(0),
                        metrics_windows(0, 1),
                        metrics_windows(0, 1, 2),
                    ),
                },
            )
            self.assertGreaterEqual(transports["target"].metrics_reads, 3)
            self.assertGreaterEqual(transports["peer"].metrics_reads, 2)
            self.assertEqual(transports["target"].perf_pids, [2001, 2001])
            first_collect = events.index("collect:perf:target")
            self.assertLess(events.index("metrics:target:2"), first_collect)
            self.assertLess(events.index("metrics:peer:1"), first_collect)

    def test_warmup_rejects_invalid_or_regressing_metrics(self) -> None:
        cases = (
            {"metrics_snapshots": (b"{bad json}\n",)},
            {"metrics_snapshots": (metrics_windows(0), metrics_windows(1))},
            {
                "metrics_snapshots": (
                    metrics_windows(1, 2),
                    metrics_windows(0, 1, 2),
                )
            },
        )
        for target_options in cases:
            with self.subTest(target_options=target_options), tempfile.TemporaryDirectory(
                prefix="pipetune-runner-"
            ) as temp_dir:
                root = pathlib.Path(temp_dir)
                with self.assertRaises(MeasureError):
                    self.run_measure(
                        root,
                        warmup_windows=3,
                        target_options=target_options,
                        peer_options={
                            "metrics_snapshots": (metrics_windows(0, 1, 2),)
                        },
                        ready_timeout_seconds=0.01,
                    )
                self.assertFalse((root / "result").exists())

    def test_warmup_detects_early_exit_and_timeout(self) -> None:
        cases = (
            {
                "target_options": {
                    "metrics_snapshots": (None,),
                    "running_checks_before_exit": 1,
                },
                "ready_timeout_seconds": 1.0,
            },
            {
                "target_options": {"metrics_snapshots": (None,)},
                "ready_timeout_seconds": 0.01,
            },
        )
        for options in cases:
            with self.subTest(options=options), tempfile.TemporaryDirectory(
                prefix="pipetune-runner-"
            ) as temp_dir:
                root = pathlib.Path(temp_dir)
                with self.assertRaises(MeasureError):
                    self.run_measure(
                        root,
                        warmup_windows=1,
                        peer_options={"metrics_snapshots": (metrics_windows(0),)},
                        **options,
                    )
                self.assertFalse((root / "result").exists())

    def test_target_server_still_starts_server_before_client(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-runner-") as temp_dir:
            root = pathlib.Path(temp_dir)
            _result, _tool, transports, events = self.run_measure(
                root, target_role="server"
            )
            starts = [event for event in events if event.startswith("start:")]
            self.assertEqual(starts, ["start:server:target", "start:client:peer"])
            self.assertGreater(transports["target"].provider_runs, 0)
            self.assertEqual(transports["peer"].provider_runs, 0)

    def test_provider_unavailability_is_not_an_endpoint_failure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-runner-") as temp_dir:
            root = pathlib.Path(temp_dir)
            result, _tool, _transports, _events = self.run_measure(
                root,
                target_options={"provider_permission_failure": True},
            )
            manifest = load_trial_manifest(
                root / "result" / "trials" / "trial-0001" / "trial.json",
                artifact_root=root / "result" / "trials" / "trial-0001",
            )
            trial_root = root / "result" / "trials" / "trial-0001"
            sample = load_metric_sample(
                trial_root / manifest.host_metrics.path,
                artifact_root=trial_root,
            )
            self.assertTrue(result.success)
            self.assertFalse(sample.counters[0].available)
            self.assertFalse(sample.counters[1].available)
            perf_commands = [
                command
                for command in sample.commands
                if "/usr/bin/perf" in command.argv and "-p" in command.argv
            ]
            self.assertEqual([command.return_code for command in perf_commands], [1, 1])

    def test_pcm_permission_failure_without_csv_is_provider_unavailable(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-runner-") as temp_dir:
            root = pathlib.Path(temp_dir)
            result, _tool, _transports, _events = self.run_measure(
                root,
                target_options={"pcm_permission_failure": True},
            )
            manifest = load_trial_manifest(
                root / "result" / "trials" / "trial-0001" / "trial.json",
                artifact_root=root / "result" / "trials" / "trial-0001",
            )
            trial_root = root / "result" / "trials" / "trial-0001"
            sample = load_metric_sample(
                trial_root / manifest.host_metrics.path,
                artifact_root=trial_root,
            )
            counters = {counter.name: counter for counter in sample.counters}
            providers = {provider.name: provider for provider in sample.providers}
            self.assertTrue(result.success)
            self.assertFalse(providers["pcm_pcie"].available)
            self.assertIn("MSR", providers["pcm_pcie"].reason or "")
            self.assertFalse(counters["io_read"].available)
            self.assertFalse(counters["io_write"].available)
            pcm_command = next(
                command
                for command in sample.commands
                if "/usr/sbin/pcm-pcie" in command.argv
            )
            self.assertEqual(pcm_command.return_code, 1)
            csv_ref = next(
                artifact
                for artifact in sample.raw_artifacts
                if artifact.path.endswith("pcm-pcie.csv")
            )
            self.assertEqual(csv_ref.size_bytes, 0)

    def test_pcm_timeout_with_complete_csv_preserves_counters(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-runner-") as temp_dir:
            root = pathlib.Path(temp_dir)
            result, _tool, _transports, _events = self.run_measure(
                root,
                target_options={"pcm_timeout_with_data": True},
            )
            trial_root = root / "result" / "trials" / "trial-0001"
            manifest = load_trial_manifest(
                trial_root / "trial.json",
                artifact_root=trial_root,
            )
            sample = load_metric_sample(
                trial_root / manifest.host_metrics.path,
                artifact_root=trial_root,
            )
            counters = {counter.name: counter for counter in sample.counters}

            self.assertTrue(result.success)
            self.assertTrue(counters["io_read"].available)
            self.assertEqual(counters["io_read"].rate_percent, 10.0)
            self.assertTrue(counters["io_write"].available)
            self.assertEqual(counters["io_write"].rate_percent, 10.0)

    def test_readiness_endpoint_and_retrieval_failures_cleanup_without_publish(self) -> None:
        cases = (
            ({"peer_options": {"start_failure": True}},),
            ({"target_options": {"endpoint_return_code": 1}},),
            ({"target_options": {"retrieval_failure": True}},),
            ({"target_options": {"provider_retrieval_failure": True}},),
            ({"peer_options": {"upload_failure": True}},),
        )
        for (options,) in cases:
            with self.subTest(options=options), tempfile.TemporaryDirectory(
                prefix="pipetune-runner-"
            ) as temp_dir:
                root = pathlib.Path(temp_dir)
                with self.assertRaises(MeasureError):
                    self.run_measure(root, **options)
                self.assertFalse((root / "result").exists())
                self.assertEqual(list(root.glob(".result.*.tmp")), [])
                self.assertTrue(
                    all(transport.cleaned == 1 for transport in self.last_transports.values())
                )

    def test_production_config_tool_requires_fingerprint_contract(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-config-tool-") as temp_dir:
            root = pathlib.Path(temp_dir)
            fake = root / "axio-configure"
            fake.write_text(
                "#!/usr/bin/env python3\n"
                "import json, sys\n"
                "if sys.argv[1] == 'fingerprints':\n"
                " print(json.dumps({'build':'b','datapath':'d','deployment':'p'}))\n"
                "else: raise SystemExit(2)\n",
                encoding="utf-8",
            )
            fake.chmod(0o755)
            tool = AxioConfigTool(fake)
            self.assertEqual(
                tool.fingerprints(root / "config.toml"),
                {"build": "b", "datapath": "d", "deployment": "p"},
            )
            source = config_document("client")
            changed = copy.deepcopy(source)
            changed["knobs"]["runtime"]["application_core_count"] = 3
            with self.assertRaises(MeasureError):
                tool.assert_runner_only_changes(source, changed)

    def test_production_config_tool_materializes_canonical_profile_command(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory(
            prefix="pipetune-config-tool-"
        ) as temp_dir:
            root = pathlib.Path(temp_dir)
            invocation = root / "invocation.json"
            fake = root / "axio-configure"
            fake.write_text(
                "#!/usr/bin/env python3\n"
                "import json, pathlib, sys\n"
                f"pathlib.Path({str(invocation)!r}).write_text(json.dumps(sys.argv[1:]))\n",
                encoding="utf-8",
            )
            fake.chmod(0o755)
            tool = AxioConfigTool(fake)
            target_output = root / "nested" / "target.toml"
            peer_output = root / "peer-nested" / "peer.toml"

            tool.materialize_target_profile_pair(
                target_input=root / "target.toml",
                peer_input=root / "peer.toml",
                target_output=target_output,
                peer_output=peer_output,
                profile="split-1to1",
                overrides={
                    "knobs.runtime.dispatcher_queue_count": 8,
                    "knobs.runtime.application_core_count": 8,
                },
            )

            self.assertTrue(target_output.parent.is_dir())
            self.assertTrue(peer_output.parent.is_dir())
            self.assertEqual(
                json.loads(invocation.read_text(encoding="utf-8")),
                [
                    "materialize-target-profile-pair",
                    str(root / "target.toml"),
                    str(root / "peer.toml"),
                    str(target_output),
                    str(peer_output),
                    "--profile",
                    "split-1to1",
                    "--target-set-json",
                    '{"knobs.runtime.application_core_count":8,'
                    '"knobs.runtime.dispatcher_queue_count":8}',
                ],
            )


if __name__ == "__main__":
    unittest.main()
