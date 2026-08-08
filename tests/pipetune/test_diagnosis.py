from __future__ import annotations

import copy
import json
import pathlib
import tempfile
import unittest

from pipetune.artifacts import (
    artifact_ref,
    write_json_atomic,
    write_metric_sample,
    write_session_manifest,
    write_trial_manifest,
)
from pipetune.diagnosis import DiagnosisError, summarize_session
from pipetune.model import (
    CounterValue,
    EndpointSpec,
    FingerprintSet,
    MetricSample,
    ProcessResult,
    ProviderStatus,
    SessionManifest,
    TrialEndpoint,
    TrialManifest,
)


ROOT = pathlib.Path(__file__).resolve().parents[2]
WINDOW_TEMPLATE = json.loads(
    (ROOT / "tests/metrics/fixtures/valid-window.jsonl").read_text()
)
START = "2026-08-08T00:00:00+00:00"
END = "2026-08-08T00:00:10+00:00"


def window(
    window_id: int,
    *,
    throughput: float = 20.0,
    stages: dict[str, tuple[float, float] | float] | None = None,
    drop_count: int = 0,
) -> dict[str, object]:
    document = copy.deepcopy(WINDOW_TEMPLATE)
    document["window_id"] = window_id
    document["throughput"]["e2e_mpps"] = throughput
    document["latency"] = {"p50_us": 1.0, "p99_us": 2.0, "p999_us": 3.0}
    defaults: dict[str, tuple[float, float] | float] = {
        "app_tx": (0.03, 0.01),
        "app_rx": (0.04, 0.01),
        "dispatcher_tx": (0.02, 0.01),
        "dispatcher_rx": (0.03, 0.01),
        "nic_tx": 0.02,
    }
    defaults.update(stages or {})
    for name in ("app_tx", "app_rx", "dispatcher_tx", "dispatcher_rx"):
        completion, stall = defaults[name]
        document["stages"][name] = {
            "completion_time_per_packet_us": completion,
            "stall_time_per_packet_us": stall,
        }
    document["stages"]["nic_tx"] = {
        "throughput_mpps": throughput,
        "submit_time_per_packet_us": defaults["nic_tx"],
    }
    document["stages"]["nic_rx"] = {
        "throughput_mpps": throughput,
        "completion_interval_cycles": 100.0 + window_id,
        "completion_interval_ns": 40.0 + window_id,
        "slowest_interval_cycles": 400.0 + window_id,
        "capacity_interval_cycles": 100.0,
    }
    document["counters"] = {
        "app_enqueue_drop_count": drop_count,
        "dispatcher_enqueue_drop_count": 0,
        "nic_rx_completion_error_count": 0,
    }
    return document


def _write_jsonl(path: pathlib.Path, windows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        "".join(json.dumps(item, separators=(",", ":")) + "\n" for item in windows)
    )


def build_session(
    root: pathlib.Path,
    *,
    target_role: str = "client",
    target_windows: list[dict[str, object]] | None = None,
    peer_windows: list[dict[str, object]] | None = None,
    warmup_windows: int = 1,
    sample_windows: int = 3,
) -> pathlib.Path:
    session_root = root / "session"
    trial_root = session_root / "trials" / "trial-0001"
    trial_root.mkdir(parents=True)
    target_windows = target_windows or [window(index) for index in range(4)]
    peer_windows = peer_windows or [window(index) for index in range(4)]
    roles = {
        "target": target_role,
        "peer": "server" if target_role == "client" else "client",
    }
    endpoints = []
    noise = {
        "throughput_relative_floor": 0.01,
        "latency_relative_floor": 0.03,
        "stage_time_relative_floor": 0.05,
        "stall_time_relative_floor": 0.05,
        "miss_rate_percentage_point_floor": 0.5,
    }
    for endpoint_id, windows in (
        ("target", target_windows),
        ("peer", peer_windows),
    ):
        metrics = trial_root / "endpoints" / endpoint_id / "metrics.jsonl"
        stdout = metrics.with_name("stdout.log")
        stderr = metrics.with_name("stderr.log")
        _write_jsonl(metrics, windows)
        stdout.write_text("ok\n")
        stderr.write_text("")
        source = trial_root / "configs" / "source" / f"{endpoint_id}.toml"
        materialized = (
            trial_root / "configs" / "materialized" / f"{endpoint_id}.toml"
        )
        canonical = trial_root / "configs" / "canonical" / f"{endpoint_id}.json"
        source.parent.mkdir(parents=True, exist_ok=True)
        materialized.parent.mkdir(parents=True, exist_ok=True)
        canonical.parent.mkdir(parents=True, exist_ok=True)
        source.write_text(f'role = "{roles[endpoint_id]}"\n')
        materialized.write_text(source.read_text())
        write_json_atomic(
            canonical,
            {
                "deployment": {
                    "role": roles[endpoint_id],
                    "topology": {
                        "application_workspaces": [2, 3, 4, 5],
                        "dispatcher_workspaces": [0, 1],
                    },
                },
                "knobs": {
                    "runtime": {
                        "application_core_count": 2,
                        "dispatcher_queue_count": 2,
                    }
                },
                "tuning": {
                    "warmup_windows": warmup_windows,
                    "sample_windows": sample_windows,
                    "noise": noise,
                },
            },
        )
        endpoints.append(
            TrialEndpoint(
                spec=EndpointSpec(
                    endpoint_id=endpoint_id,
                    role=roles[endpoint_id],
                    backend="dpdk",
                    transport="local",
                    host="",
                    ssh_port=0,
                    ssh_user="",
                    workdir="/opt/axio",
                    use_sudo=True,
                    numa_node=0,
                ),
                fingerprints=FingerprintSet(
                    git_commit="b" * 40,
                    binary_sha256=("a" if endpoint_id == "target" else "d") * 64,
                    source_config_sha256=artifact_ref(trial_root, source).sha256,
                    build=f"build-{endpoint_id}",
                    datapath=f"datapath-{endpoint_id}",
                    deployment=f"deployment-{endpoint_id}",
                ),
                process=ProcessResult(
                    argv=("./axio", "--config", str(materialized)),
                    status="exited",
                    return_code=0,
                    failure_reason=None,
                    started_at_utc=START,
                    ended_at_utc=END,
                    stdout=artifact_ref(trial_root, stdout),
                    stderr=artifact_ref(trial_root, stderr),
                ),
                artifacts=(
                    artifact_ref(trial_root, metrics, schema="axio.metrics/v1"),
                    artifact_ref(trial_root, source),
                    artifact_ref(trial_root, materialized),
                    artifact_ref(trial_root, canonical),
                ),
            )
        )
    raw = trial_root / "providers" / "raw.txt"
    raw.parent.mkdir()
    raw.write_text("typed provider evidence\n")
    counters = tuple(
        CounterValue(
            name=name,
            available=True,
            numerator=10.0,
            denominator=100.0,
            rate_percent=10.0,
            reason=None,
            samples_percent=(9.0, 10.0, 11.0),
        )
        for name in ("llc_load", "llc_store", "io_read", "io_write")
    )
    host_metrics = trial_root / "host-metrics.json"
    write_metric_sample(
        host_metrics,
        MetricSample(
            schema="pipetune.host-metrics/v1",
            sample_id="target-host-metrics",
            endpoint_id="target",
            started_at_utc=START,
            ended_at_utc=END,
            sample_interval_seconds=float(sample_windows),
            socket_id=0,
            counters=counters,
            providers=(
                ProviderStatus("perf", True, "/usr/bin/perf", "perf 1", None),
                ProviderStatus("pcm_pcie", True, "/usr/bin/pcm", "pcm 1", None),
            ),
            commands=(),
            raw_artifacts=(artifact_ref(trial_root, raw),),
        ),
    )
    manifest = TrialManifest(
        schema="pipetune.trial/v1",
        trial_id="trial-0001",
        target_endpoint_id="target",
        started_at_utc=START,
        ended_at_utc=END,
        status="success",
        cleanup_status="clean",
        endpoints=tuple(endpoints),
        host_metrics=artifact_ref(
            trial_root, host_metrics, schema="pipetune.host-metrics/v1"
        ),
        failure_reason=None,
    )
    trial_path = trial_root / "trial.json"
    write_trial_manifest(trial_path, manifest)
    write_session_manifest(
        session_root / "session.json",
        SessionManifest(
            schema="pipetune.session/v1",
            session_id="session-0001",
            started_at_utc=START,
            ended_at_utc=END,
            status="complete",
            trials=(
                artifact_ref(session_root, trial_path, schema="pipetune.trial/v1"),
            ),
            failure_reason=None,
        ),
    )
    return session_root


class SteadySummaryTest(unittest.TestCase):
    def test_removes_warmup_and_summarizes_native_independent_series(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target = [
                window(0, stages={"app_rx": (9.0, 0.01)}),
                window(1, stages={"app_rx": (1.0, 0.01)}),
                window(2, stages={"app_rx": (2.0, 0.01)}),
                window(3, stages={"app_rx": (100.0, 0.01)}),
            ]
            summary = summarize_session(
                build_session(root, target_windows=target), trial_id="trial-0001"
            )
            app_rx = summary.target.component("app_rx.completion")
            self.assertEqual(app_rx.statistic.samples, (1.0, 2.0, 100.0))
            self.assertEqual(app_rx.statistic.median, 2.0)
            self.assertEqual(app_rx.statistic.mad, 1.0)
            self.assertEqual(app_rx.statistic.uncertainty, 3.0)
            nic_rx = summary.target.component("nic_rx.aggregate")
            self.assertAlmostEqual(nic_rx.statistic.median, 0.05)
            self.assertEqual(
                summary.target.supporting["nic_rx.completion_interval_ns"].median,
                42.0,
            )
            self.assertEqual(summary.counters["llc_load"].median, 10.0)
            self.assertEqual(summary.counters["llc_load"].mad, 1.0)
            self.assertTrue(summary.peer_health.healthy)
            self.assertIn("session.json", summary.input_hashes)
            self.assertIn("providers/raw.txt", summary.input_hashes)

    def test_explicitly_marks_an_uncertain_stage_ranking_ambiguous(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            root = pathlib.Path(temp_dir)
            target = [
                window(
                    index,
                    stages={"app_rx": (0.100, 0.01), "app_tx": (0.099, 0.01)},
                )
                for index in range(4)
            ]
            summary = summarize_session(build_session(root, target_windows=target))
            self.assertIsNone(summary.target.dominant_component)
            self.assertEqual(summary.target.ranking_status, "ambiguous")

    def test_peer_health_rejects_drops_throughput_gap_and_source_dominance(self) -> None:
        cases = {
            "drop": [window(index, drop_count=1 if index == 2 else 0) for index in range(4)],
            "throughput": [window(index, throughput=10.0) for index in range(4)],
            "source": [
                window(index, stages={"app_tx": (2.0, 0.01)})
                for index in range(4)
            ],
        }
        for reason, peer in cases.items():
            with self.subTest(reason=reason), tempfile.TemporaryDirectory(
                prefix="pipetune-diagnosis-"
            ) as temp_dir:
                summary = summarize_session(
                    build_session(pathlib.Path(temp_dir), peer_windows=peer)
                )
                self.assertFalse(summary.peer_health.healthy)
                self.assertTrue(
                    any(reason in item for item in summary.peer_health.reasons),
                    summary.peer_health.reasons,
                )

    def test_records_request_or_response_source_for_both_target_roles(self) -> None:
        for role, source in (("server", "request"), ("client", "response")):
            with self.subTest(role=role), tempfile.TemporaryDirectory(
                prefix="pipetune-diagnosis-"
            ) as temp_dir:
                summary = summarize_session(
                    build_session(pathlib.Path(temp_dir), target_role=role)
                )
                self.assertEqual(summary.peer_health.traffic_source, source)

    def test_rejects_missing_windows_hash_mismatch_and_lifecycle_failure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            root = pathlib.Path(temp_dir)
            missing = build_session(root, target_windows=[window(0), window(1)])
            with self.assertRaises(DiagnosisError):
                summarize_session(missing)

        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            root = pathlib.Path(temp_dir)
            session = build_session(root)
            metrics = session / "trials/trial-0001/endpoints/target/metrics.jsonl"
            metrics.write_text("tampered\n")
            with self.assertRaises(DiagnosisError):
                summarize_session(session)

        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            root = pathlib.Path(temp_dir)
            session = build_session(root)
            trial = session / "trials/trial-0001/trial.json"
            document = json.loads(trial.read_text())
            document["endpoints"][0]["process"]["return_code"] = 1
            write_json_atomic(trial, document)
            session_document = json.loads((session / "session.json").read_text())
            session_document["trials"][0] = {
                "path": "trials/trial-0001/trial.json",
                "schema": "pipetune.trial/v1",
                "sha256": artifact_ref(session, trial).sha256,
                "size_bytes": trial.stat().st_size,
            }
            write_json_atomic(session / "session.json", session_document)
            with self.assertRaises(DiagnosisError):
                summarize_session(session)


if __name__ == "__main__":
    unittest.main()
