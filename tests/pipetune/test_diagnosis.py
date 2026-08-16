from __future__ import annotations

import copy
import dataclasses
import json
import pathlib
import shutil
import tempfile
import unittest

from pipetune.artifacts import (
    artifact_ref,
    sha256_file,
    write_json_atomic,
    write_metric_sample,
    write_session_manifest,
    write_trial_manifest,
)
from pipetune.diagnosis import (
    DiagnosisError,
    diagnose_summary,
    diagnosis_document,
    publish_diagnosis,
    summarize_session,
)
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
    counter_samples: dict[str, tuple[float, ...] | None] | None = None,
    application_core_count: int = 2,
    dispatcher_queue_count: int = 2,
    application_workspaces: tuple[int, ...] = (2, 3, 4, 5),
    git_commit: str = "b" * 40,
    request_payload_bytes: int | None = None,
    response_payload_bytes: int | None = None,
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
        endpoint_application_count = (
            application_core_count if endpoint_id == "target" else 2
        )
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
        canonical_document = {
                "deployment": {
                    "role": roles[endpoint_id],
                    "topology": {
                        "application_workspaces": list(application_workspaces),
                        "dispatcher_workspaces": [0, 1],
                        "workloads": [
                            {
                                "id": 1,
                                "groups": [
                                    {
                                        "dispatcher": 0,
                                        "applications": list(
                                            application_workspaces[
                                                :endpoint_application_count
                                            ]
                                        ),
                                    }
                                ],
                                "remote_dispatchers": [0],
                            }
                        ],
                    },
                },
                "knobs": {
                    "runtime": {
                        "application_core_count": endpoint_application_count,
                        "dispatcher_queue_count": dispatcher_queue_count,
                        "nic_rx_post_size": 32,
                    }
                },
                "tuning": {
                    "warmup_windows": warmup_windows,
                    "sample_windows": sample_windows,
                    "noise": noise,
                },
            }
        if request_payload_bytes is not None or response_payload_bytes is not None:
            canonical_document["handler"] = {
                "request_payload_bytes": request_payload_bytes,
                "response_payload_bytes": response_payload_bytes,
            }
        write_json_atomic(canonical, canonical_document)
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
                    git_commit=git_commit,
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
    counter_samples = counter_samples or {
        name: (9.0, 10.0, 11.0)
        for name in ("llc_load", "llc_store", "io_read", "io_write")
    }
    counters = []
    for name in ("llc_load", "llc_store", "io_read", "io_write"):
        samples = counter_samples.get(name)
        if samples is None:
            counters.append(
                CounterValue(name, False, None, None, None, "not available")
            )
            continue
        rate = sum(samples) / len(samples)
        counters.append(
            CounterValue(
                name=name,
                available=True,
                numerator=rate,
                denominator=100.0,
                rate_percent=rate,
                reason=None,
                samples_percent=samples,
            )
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
            counters=tuple(counters),
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

    def test_peer_tx_dominance_is_expected_for_a_larger_request(self) -> None:
        peer = [
            window(index, stages={"app_tx": (2.0, 0.01)})
            for index in range(4)
        ]
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            summary = summarize_session(
                build_session(
                    pathlib.Path(temp_dir),
                    target_role="server",
                    peer_windows=peer,
                    request_payload_bytes=470,
                    response_payload_bytes=22,
                )
            )

        self.assertTrue(summary.peer_health.healthy)

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


class LongestComponentDiagnosisTest(unittest.TestCase):
    @staticmethod
    def _colocated_completion_summary(
        root: pathlib.Path, *, count: int, budget: int = 16
    ):
        target = [
            window(index, stages={"app_rx": (0.20, 0.01)})
            for index in range(4)
        ]
        summary = summarize_session(
            build_session(
                root,
                target_windows=target,
                application_core_count=count,
                dispatcher_queue_count=count,
                application_workspaces=tuple(range(budget)),
            )
        )
        canonical_target = copy.deepcopy(summary.canonical_target)
        canonical_target["deployment"]["topology"] = {
            "application_workspaces": list(range(budget)),
            "dispatcher_workspaces": list(range(budget)),
            "workloads": [
                {
                    "id": 1,
                    "groups": [
                        {"dispatcher": index, "applications": [index]}
                        for index in range(count)
                    ],
                    "remote_dispatchers": list(range(8)),
                }
            ],
            "workspaces": [
                {"id": index, "cpu_core": index} for index in range(budget)
            ],
        }
        canonical_target["knobs"]["runtime"][
            "application_core_count"
        ] = count
        canonical_target["knobs"]["runtime"][
            "dispatcher_queue_count"
        ] = count
        return dataclasses.replace(summary, canonical_target=canonical_target)

    def _diagnose(
        self,
        root: pathlib.Path,
        *,
        target_stages: dict[str, tuple[float, float] | float],
        throughput: float = 20.0,
        counter_samples: dict[str, tuple[float, ...] | None] | None = None,
    ):
        target = [
            window(index, throughput=throughput, stages=target_stages)
            for index in range(4)
        ]
        peer = [window(index, throughput=throughput) for index in range(4)]
        return diagnose_summary(
            summarize_session(
                build_session(
                    root,
                    target_windows=target,
                    peer_windows=peer,
                    counter_samples=counter_samples,
                )
            )
        )

    def test_dominant_pipeline_stall_is_p1(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            diagnosis = self._diagnose(
                pathlib.Path(temp_dir),
                target_stages={"app_tx": (0.03, 1.0)},
            )
            self.assertEqual((diagnosis.point, diagnosis.direction), ("P1", "tx"))
            self.assertIn(diagnosis.confidence, ("high", "medium", "low"))
            self.assertEqual(diagnosis.evidence[0].name, "app_tx.stall")

    def test_fully_colocated_completion_requires_paired_reduction(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            summary = self._colocated_completion_summary(
                pathlib.Path(temp_dir), count=16
            )

            diagnosis = diagnose_summary(summary)

            self.assertEqual(diagnosis.point, "paired_reduction_required")
            self.assertEqual(diagnosis.confidence, "none")
            self.assertEqual(
                dataclasses.asdict(diagnosis.required_paired_reduction),
                {
                    "baseline_application_count": 16,
                    "baseline_dispatcher_count": 16,
                    "candidate_application_count": 15,
                    "candidate_dispatcher_count": 15,
                },
            )
            self.assertIsNone(diagnosis.required_probe)
            document = diagnosis_document(diagnosis, summary)
            self.assertEqual(
                document["result"]["required_paired_reduction"],
                {
                    "baseline_application_count": 16,
                    "baseline_dispatcher_count": 16,
                    "candidate_application_count": 15,
                    "candidate_dispatcher_count": 15,
                },
            )

    def test_single_colocated_pair_transitions_to_compute_evidence(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            summary = self._colocated_completion_summary(
                pathlib.Path(temp_dir), count=1
            )

            diagnosis = diagnose_summary(summary)

            self.assertEqual(diagnosis.point, "inconclusive")
            self.assertIsNone(diagnosis.required_probe)
            self.assertIsNone(diagnosis.required_paired_reduction)
            self.assertTrue(
                any(
                    "paired colocated reduction is exhausted" in item.reason
                    for item in diagnosis.evidence
                )
            )

    def test_dominant_tx_or_rx_nic_is_p3(self) -> None:
        cases = (
            ({"nic_tx": 1.0}, 20.0, "tx", "nic_tx.submit"),
            ({}, 1.0, "rx", "nic_rx.aggregate"),
        )
        for stages, throughput, direction, component in cases:
            with self.subTest(direction=direction), tempfile.TemporaryDirectory(
                prefix="pipetune-diagnosis-"
            ) as temp_dir:
                diagnosis = self._diagnose(
                    pathlib.Path(temp_dir),
                    target_stages=stages,
                    throughput=throughput,
                )
                self.assertEqual((diagnosis.point, diagnosis.direction), ("P3", direction))
                self.assertEqual(diagnosis.evidence[0].name, component)

    def test_tie_is_inconclusive_without_using_counters_to_override_it(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            diagnosis = self._diagnose(
                pathlib.Path(temp_dir),
                target_stages={"app_tx": (0.5, 0.01), "app_rx": (0.5, 0.01)},
            )
            self.assertEqual(diagnosis.point, "inconclusive")
            self.assertIsNone(diagnosis.direction)
            self.assertEqual(diagnosis.confidence, "none")

    def test_all_four_rates_are_missing_or_evaluated_and_conflicts_lower_confidence(self) -> None:
        samples = {
            "llc_load": (20.0, 20.0, 20.0),
            "llc_store": (10.0, 10.0, 10.0),
            "io_read": (10.0, 10.0, 10.0),
            "io_write": (30.0, 30.0, 30.0),
        }
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            diagnosis = self._diagnose(
                pathlib.Path(temp_dir),
                target_stages={"nic_tx": 1.0},
                counter_samples=samples,
            )
            self.assertEqual(diagnosis.point, "P3")
            self.assertEqual(diagnosis.confidence, "low")
            rate_names = {
                item.name
                for item in (*diagnosis.evidence, *diagnosis.rejected_evidence)
                if item.kind == "counter"
            }
            self.assertEqual(
                rate_names, {"llc_load", "llc_store", "io_read", "io_write"}
            )
            self.assertTrue(
                any("conflict" in item.reason for item in diagnosis.rejected_evidence)
            )

        missing = dict(samples)
        missing["io_read"] = None
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            diagnosis = self._diagnose(
                pathlib.Path(temp_dir),
                target_stages={"nic_tx": 1.0},
                counter_samples=missing,
            )
            self.assertIn("io_read", diagnosis.missing_metrics)
            self.assertEqual(diagnosis.confidence, "low")

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


class CorePerturbationDiagnosisTest(unittest.TestCase):
    def _summary(
        self,
        root: pathlib.Path,
        *,
        count: int,
        capacity: int = 4,
        dispatcher_count: int = 2,
        llc_store: tuple[float, ...] | None = (10.0, 10.0, 10.0),
        io_read: tuple[float, ...] | None = (10.0, 10.0, 10.0),
        llc_load: tuple[float, ...] | None = (0.0, 0.0, 0.0),
        io_write: tuple[float, ...] | None = (0.0, 0.0, 0.0),
        git_commit: str = "b" * 40,
    ):
        target = [
            window(index, stages={"app_tx": (1.0, 0.01)})
            for index in range(4)
        ]
        return summarize_session(
            build_session(
                root,
                target_windows=target,
                counter_samples={
                    "llc_load": llc_load,
                    "llc_store": llc_store,
                    "io_read": io_read,
                    "io_write": io_write,
                },
                application_core_count=count,
                dispatcher_queue_count=dispatcher_count,
                application_workspaces=tuple(range(2, 2 + capacity)),
                git_commit=git_commit,
            )
        )

    def test_emits_plus_one_or_maximum_core_minus_one_probe(self) -> None:
        cases = ((2, 4, 1, 3), (4, 4, -1, 3))
        for count, capacity, direction, candidate in cases:
            with self.subTest(count=count), tempfile.TemporaryDirectory(
                prefix="pipetune-diagnosis-"
            ) as temp_dir:
                diagnosis = diagnose_summary(
                    self._summary(
                        pathlib.Path(temp_dir), count=count, capacity=capacity
                    )
                )
                self.assertEqual(diagnosis.point, "probe_required")
                self.assertIsNotNone(diagnosis.required_probe)
                self.assertEqual(diagnosis.required_probe.direction, direction)
                self.assertEqual(diagnosis.required_probe.candidate_value, candidate)

    def test_reports_resource_exhaustion_when_no_legal_c1_probe_exists(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            diagnosis = diagnose_summary(
                self._summary(
                    pathlib.Path(temp_dir), count=2, capacity=2, dispatcher_count=2
                )
            )
            self.assertEqual(diagnosis.point, "inconclusive")
            self.assertIsNone(diagnosis.required_probe)
            self.assertIn("c1_probe_capacity", diagnosis.missing_metrics)

    def test_positive_normalized_llc_slope_is_p2_for_plus_or_minus_probe(self) -> None:
        cases = (
            (2, 4, (10.0, 10.0, 10.0), 3, (12.0, 12.0, 12.0)),
            (4, 4, (12.0, 12.0, 12.0), 3, (10.0, 10.0, 10.0)),
        )
        for base_count, capacity, base_llc, probe_count, probe_llc in cases:
            with self.subTest(base_count=base_count), tempfile.TemporaryDirectory(
                prefix="pipetune-base-"
            ) as base_dir, tempfile.TemporaryDirectory(
                prefix="pipetune-probe-"
            ) as probe_dir:
                baseline = self._summary(
                    pathlib.Path(base_dir),
                    count=base_count,
                    capacity=capacity,
                    llc_store=base_llc,
                )
                probe = self._summary(
                    pathlib.Path(probe_dir),
                    count=probe_count,
                    capacity=capacity,
                    llc_store=probe_llc,
                )
                diagnosis = diagnose_summary(baseline, probe_summary=probe)
                self.assertEqual(diagnosis.point, "P2")
                slope = next(
                    item for item in diagnosis.evidence if item.name == "llc_slope"
                )
                self.assertGreater(slope.value or 0.0, slope.uncertainty or 0.0)

    def test_zero_or_negative_llc_slope_with_strong_io_is_p4(self) -> None:
        cases = (
            ((10.0, 10.0, 10.0), (10.0, 10.0, 10.0)),
            ((10.0, 10.0, 10.0), (8.0, 8.0, 8.0)),
        )
        for baseline_llc, probe_llc in cases:
            with self.subTest(probe_llc=probe_llc), tempfile.TemporaryDirectory(
                prefix="pipetune-base-"
            ) as base_dir, tempfile.TemporaryDirectory(
                prefix="pipetune-probe-"
            ) as probe_dir:
                baseline = self._summary(
                    pathlib.Path(base_dir), count=2, llc_store=baseline_llc
                )
                probe = self._summary(
                    pathlib.Path(probe_dir), count=3, llc_store=probe_llc
                )
                diagnosis = diagnose_summary(baseline, probe_summary=probe)
                self.assertEqual(diagnosis.point, "P4")

    def test_noise_missing_counters_and_io_conflict_remain_inconclusive(self) -> None:
        cases = (
            {"io_read": (0.0, 0.0, 0.0)},
            {"llc_store": None},
            {"io_write": (20.0, 20.0, 20.0)},
        )
        for changes in cases:
            with self.subTest(changes=changes), tempfile.TemporaryDirectory(
                prefix="pipetune-base-"
            ) as base_dir, tempfile.TemporaryDirectory(
                prefix="pipetune-probe-"
            ) as probe_dir:
                baseline = self._summary(pathlib.Path(base_dir), count=2, **changes)
                probe = self._summary(pathlib.Path(probe_dir), count=3, **changes)
                diagnosis = diagnose_summary(baseline, probe_summary=probe)
                self.assertEqual(diagnosis.point, "inconclusive")

    def test_rejects_probe_with_mismatched_artifacts_or_wrong_candidate(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="pipetune-base-"
        ) as base_dir, tempfile.TemporaryDirectory(
            prefix="pipetune-probe-"
        ) as probe_dir:
            baseline = self._summary(pathlib.Path(base_dir), count=2)
            wrong_revision = self._summary(
                pathlib.Path(probe_dir), count=3, git_commit="c" * 40
            )
            with self.assertRaises(DiagnosisError):
                diagnose_summary(baseline, probe_summary=wrong_revision)

        with tempfile.TemporaryDirectory(
            prefix="pipetune-base-"
        ) as base_dir, tempfile.TemporaryDirectory(
            prefix="pipetune-probe-"
        ) as probe_dir:
            baseline = self._summary(pathlib.Path(base_dir), count=2)
            wrong_count = self._summary(pathlib.Path(probe_dir), count=4)
            with self.assertRaises(DiagnosisError):
                diagnose_summary(baseline, probe_summary=wrong_count)

    def test_rejects_non_c1_config_deltas_and_peer_fingerprint_changes(self) -> None:
        mutations = (
            lambda probe: probe.canonical_target["knobs"]["runtime"].__setitem__(
                "dispatcher_queue_count", 3
            ),
            lambda probe: probe.canonical_target["knobs"]["runtime"].__setitem__(
                "nic_rx_post_size", 64
            ),
            lambda probe: probe.canonical_target.__setitem__(
                "network", {"backend": "roce"}
            ),
            lambda probe: probe.canonical_target["tuning"]["noise"].__setitem__(
                "throughput_relative_floor", 0.2
            ),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory(
                prefix="pipetune-base-"
            ) as base_dir, tempfile.TemporaryDirectory(
                prefix="pipetune-probe-"
            ) as probe_dir:
                baseline = self._summary(pathlib.Path(base_dir), count=2)
                probe = self._summary(pathlib.Path(probe_dir), count=3)
                mutation(probe)
                with self.assertRaises(DiagnosisError):
                    diagnose_summary(baseline, probe_summary=probe)

        with tempfile.TemporaryDirectory(
            prefix="pipetune-base-"
        ) as base_dir, tempfile.TemporaryDirectory(
            prefix="pipetune-probe-"
        ) as probe_dir:
            baseline = self._summary(pathlib.Path(base_dir), count=2)
            probe = self._summary(pathlib.Path(probe_dir), count=3)
            changed_peer = dataclasses.replace(
                probe,
                peer_fingerprints=dataclasses.replace(
                    probe.peer_fingerprints, datapath="changed-peer-datapath"
                ),
            )
            with self.assertRaises(DiagnosisError):
                diagnose_summary(baseline, probe_summary=changed_peer)

    def test_rejects_unhealthy_probe_evidence(self) -> None:
        reasons = (
            "drop: peer app enqueue",
            "throughput: endpoint medians differ",
            "source: peer traffic-source TX path dominates the target",
        )
        for reason in reasons:
            with self.subTest(reason=reason), tempfile.TemporaryDirectory(
                prefix="pipetune-base-"
            ) as base_dir, tempfile.TemporaryDirectory(
                prefix="pipetune-probe-"
            ) as probe_dir:
                baseline = self._summary(pathlib.Path(base_dir), count=2)
                probe = self._summary(pathlib.Path(probe_dir), count=3)
                probe = dataclasses.replace(
                    probe,
                    peer_health=dataclasses.replace(
                        probe.peer_health, healthy=False, reasons=(reason,)
                    ),
                )
                diagnosis = diagnose_summary(baseline, probe_summary=probe)
                self.assertEqual(diagnosis.point, "peer_unhealthy")
                self.assertIsNone(diagnosis.required_probe)

    def test_opposite_pair_changes_probe_confidence_or_blocks_p4(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="pipetune-base-"
        ) as base_dir, tempfile.TemporaryDirectory(
            prefix="pipetune-probe-"
        ) as probe_dir:
            baseline = self._summary(
                pathlib.Path(base_dir), count=2, io_write=(20.0, 20.0, 20.0)
            )
            probe = self._summary(
                pathlib.Path(probe_dir), count=3,
                llc_store=(12.0, 12.0, 12.0),
                io_write=(20.0, 20.0, 20.0),
            )
            diagnosis = diagnose_summary(baseline, probe_summary=probe)
            self.assertEqual(diagnosis.point, "P2")
            self.assertEqual(diagnosis.confidence, "low")
            self.assertTrue(
                any("conflict" in item.reason for item in diagnosis.rejected_evidence)
            )

        with tempfile.TemporaryDirectory(
            prefix="pipetune-base-"
        ) as base_dir, tempfile.TemporaryDirectory(
            prefix="pipetune-probe-"
        ) as probe_dir:
            baseline = self._summary(
                pathlib.Path(base_dir), count=2, llc_load=(20.0, 20.0, 20.0)
            )
            probe = self._summary(
                pathlib.Path(probe_dir), count=3, llc_load=(20.0, 20.0, 20.0)
            )
            diagnosis = diagnose_summary(baseline, probe_summary=probe)
            self.assertEqual(diagnosis.point, "P4")
            self.assertEqual(diagnosis.confidence, "medium")

        with tempfile.TemporaryDirectory(
            prefix="pipetune-base-"
        ) as base_dir, tempfile.TemporaryDirectory(
            prefix="pipetune-probe-"
        ) as probe_dir:
            baseline = self._summary(pathlib.Path(base_dir), count=2, llc_load=None)
            probe = self._summary(pathlib.Path(probe_dir), count=3, llc_load=None)
            diagnosis = diagnose_summary(baseline, probe_summary=probe)
            self.assertEqual(diagnosis.point, "P4")
            self.assertEqual(diagnosis.confidence, "medium")

    def test_completed_probe_is_audited_but_not_requested_again(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="pipetune-base-"
        ) as base_dir, tempfile.TemporaryDirectory(
            prefix="pipetune-probe-"
        ) as probe_dir:
            baseline = self._summary(pathlib.Path(base_dir), count=2)
            probe = self._summary(
                pathlib.Path(probe_dir), count=3, llc_store=(12.0, 12.0, 12.0)
            )
            diagnosis = diagnose_summary(baseline, probe_summary=probe)
            self.assertEqual(diagnosis.point, "P2")
            self.assertIsNone(diagnosis.required_probe)
            self.assertEqual(diagnosis.completed_probe.candidate_value, 3)


class DiagnosisPublicationTest(unittest.TestCase):
    def test_document_contains_compact_auditable_statistics(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            session = build_session(pathlib.Path(temp_dir))
            summary = summarize_session(session)
            diagnosis = diagnose_summary(summary)
            document = diagnosis_document(diagnosis, summary)

            self.assertEqual(document["schema"], "pipetune.diagnosis/v1")
            self.assertEqual(document["trial"]["baseline"], "trial-0001")
            self.assertIsNone(document["trial"]["probe"])
            self.assertEqual(document["result"]["point"], diagnosis.point)
            first_stage = document["steady_state"]["target"]["stage_ranking"][0]
            self.assertIn("median", first_stage["statistic"])
            self.assertIn("mad", first_stage["statistic"])
            self.assertNotIn("samples", first_stage["statistic"])
            self.assertEqual(
                set(document["counter_rates"]["baseline"]),
                {"llc_load", "llc_store", "io_read", "io_write"},
            )
            self.assertIn("session.json", document["input_hashes"])

    def test_multiple_trials_require_explicit_selection(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            session = build_session(pathlib.Path(temp_dir))
            first_trial = session / "trials/trial-0001"
            second_trial = session / "trials/trial-0002"
            shutil.copytree(first_trial, second_trial)
            second_manifest = second_trial / "trial.json"
            document = json.loads(second_manifest.read_text())
            document["trial_id"] = "trial-0002"
            write_json_atomic(second_manifest, document)
            session_path = session / "session.json"
            session_document = json.loads(session_path.read_text())
            session_document["trials"].append(
                {
                    "path": "trials/trial-0002/trial.json",
                    "schema": "pipetune.trial/v1",
                    "sha256": artifact_ref(session, second_manifest).sha256,
                    "size_bytes": second_manifest.stat().st_size,
                }
            )
            write_json_atomic(session_path, session_document)

            with self.assertRaises(DiagnosisError):
                publish_diagnosis(session)
            publication = publish_diagnosis(session, trial_id="trial-0002")
            self.assertEqual(publication.document["trial"]["baseline"], "trial-0002")

    def test_publish_is_pretty_deterministic_and_preserves_raw_artifacts(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            session = build_session(pathlib.Path(temp_dir))
            raw_paths = tuple(
                path
                for path in session.rglob("*")
                if path.is_file()
            )
            before = {path: sha256_file(path) for path in raw_paths}

            first = publish_diagnosis(session)
            first_bytes = first.path.read_bytes()
            second = publish_diagnosis(session)

            self.assertEqual(
                first.path, session.resolve() / "diagnoses/trial-0001.json"
            )
            self.assertEqual(first_bytes, second.path.read_bytes())
            self.assertIn(b'\n  "counter_rates"', first_bytes)
            self.assertTrue(first_bytes.endswith(b"\n"))
            self.assertEqual(before, {path: sha256_file(path) for path in raw_paths})

    def test_publish_accepts_a_separate_completed_probe_session(self) -> None:
        with tempfile.TemporaryDirectory(
            prefix="pipetune-baseline-"
        ) as baseline_dir, tempfile.TemporaryDirectory(
            prefix="pipetune-probe-"
        ) as probe_dir:
            baseline = build_session(
                pathlib.Path(baseline_dir),
                target_windows=[
                    window(index, stages={"app_tx": (1.0, 0.01)})
                    for index in range(4)
                ],
                application_core_count=2,
                counter_samples={
                    "llc_load": (0.0, 0.0, 0.0),
                    "llc_store": (10.0, 10.0, 10.0),
                    "io_read": (10.0, 10.0, 10.0),
                    "io_write": (0.0, 0.0, 0.0),
                },
            )
            probe = build_session(
                pathlib.Path(probe_dir),
                target_windows=[
                    window(index, stages={"app_tx": (1.0, 0.01)})
                    for index in range(4)
                ],
                application_core_count=3,
                counter_samples={
                    "llc_load": (0.0, 0.0, 0.0),
                    "llc_store": (12.0, 12.0, 12.0),
                    "io_read": (10.0, 10.0, 10.0),
                    "io_write": (0.0, 0.0, 0.0),
                },
            )
            published = publish_diagnosis(baseline, probe_session_root=probe)
            self.assertEqual(published.document["result"]["point"], "P2")
            self.assertIsNone(published.document["result"]["required_probe"])
            self.assertEqual(
                published.document["result"]["completed_probe"]["candidate_value"],
                3,
            )
            self.assertIsNotNone(published.document["counter_rates"]["probe"])

    def test_publish_rejects_probe_when_baseline_does_not_require_one(self) -> None:
        cases = (
            {
                "target_windows": [
                    window(index, stages={"nic_tx": 1.0})
                    for index in range(4)
                ]
            },
            {
                "peer_windows": [
                    window(index, drop_count=1 if index == 2 else 0)
                    for index in range(4)
                ]
            },
            {
                "target_windows": [
                    window(index, stages={"app_tx": (1.0, 0.01)})
                    for index in range(4)
                ],
                "application_core_count": 2,
                "dispatcher_queue_count": 2,
                "application_workspaces": (2, 3),
            },
            {
                "target_windows": [
                    window(
                        index,
                        stages={"app_tx": (0.5, 0.01), "app_rx": (0.5, 0.01)},
                    )
                    for index in range(4)
                ]
            },
        )
        for baseline_arguments in cases:
            with self.subTest(
                baseline_arguments=baseline_arguments
            ), tempfile.TemporaryDirectory(
                prefix="pipetune-baseline-"
            ) as baseline_dir, tempfile.TemporaryDirectory(
                prefix="pipetune-probe-"
            ) as probe_dir:
                baseline = build_session(
                    pathlib.Path(baseline_dir), **baseline_arguments
                )
                probe = build_session(pathlib.Path(probe_dir))
                with self.assertRaisesRegex(
                    DiagnosisError, "baseline does not require"
                ):
                    publish_diagnosis(baseline, probe_session_root=probe)

    def test_probe_trial_requires_probe_session(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-diagnosis-") as temp_dir:
            session = build_session(pathlib.Path(temp_dir))
            with self.assertRaisesRegex(DiagnosisError, "requires --probe-session"):
                publish_diagnosis(session, probe_trial_id="trial-0001")


if __name__ == "__main__":
    unittest.main()
