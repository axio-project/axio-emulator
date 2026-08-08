from __future__ import annotations

import copy
import json
import pathlib
import tempfile
import unittest

from pipetune.artifacts import (
    artifact_ref,
    load_metric_sample,
    load_session_manifest,
    load_trial_manifest,
    sha256_file,
    write_json_atomic,
    write_metric_sample,
    write_session_manifest,
    write_trial_manifest,
)
from pipetune.model import (
    ArtifactRef,
    ContractError,
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


GIT_SHA = "b" * 40
START = "2026-08-08T00:00:00+00:00"
END = "2026-08-08T00:00:01+00:00"


def endpoint(root: pathlib.Path, endpoint_id: str, role: str) -> TrialEndpoint:
    endpoint_dir = root / endpoint_id
    endpoint_dir.mkdir()
    stdout = endpoint_dir / "stdout.txt"
    stderr = endpoint_dir / "stderr.txt"
    metrics = endpoint_dir / "metrics.jsonl"
    stdout.write_text("stdout\n")
    stderr.write_text("")
    metrics.write_text("{}\n")
    return TrialEndpoint(
        spec=EndpointSpec(
            endpoint_id=endpoint_id,
            role=role,
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
            git_commit=GIT_SHA,
            binary_sha256="a" * 64,
            source_config_sha256="c" * 64,
            build="fnv1a64:0000000000000001",
            datapath="fnv1a64:0000000000000002",
            deployment="fnv1a64:0000000000000003",
        ),
        process=ProcessResult(
            argv=("./axio",),
            status="exited",
            return_code=0,
            failure_reason=None,
            started_at_utc=START,
            ended_at_utc=END,
            stdout=artifact_ref(root, stdout),
            stderr=artifact_ref(root, stderr),
        ),
        artifacts=(
            artifact_ref(root, metrics, schema="axio.metrics/v1"),
        ),
    )


class ArtifactContractTest(unittest.TestCase):
    def test_atomic_json_is_readable_deterministic_and_finite(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-artifacts-") as temp_dir:
            root = pathlib.Path(temp_dir)
            path = root / "nested" / "value.json"
            write_json_atomic(path, {"z": 1, "a": {"b": 2}})
            first = path.read_bytes()
            self.assertEqual(first, b'{\n  "a": {\n    "b": 2\n  },\n  "z": 1\n}\n')
            write_json_atomic(path, {"a": {"b": 2}, "z": 1})
            self.assertEqual(path.read_bytes(), first)
            self.assertEqual(list(path.parent.glob(f".{path.name}.*.tmp")), [])
            with self.assertRaises((ValueError, ContractError)):
                write_json_atomic(path, {"value": float("nan")})
            self.assertEqual(path.read_bytes(), first)

    def test_manifest_round_trip_and_hash_verification(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-manifest-") as temp_dir:
            root = pathlib.Path(temp_dir)
            host_metrics = root / "target-host-metrics.json"
            host_metrics.write_text("{}\n")
            manifest = TrialManifest(
                schema="pipetune.trial/v1",
                trial_id="trial-0001",
                target_endpoint_id="target",
                started_at_utc=START,
                ended_at_utc=END,
                status="success",
                cleanup_status="clean",
                endpoints=(
                    endpoint(root, "target", "client"),
                    endpoint(root, "peer", "server"),
                ),
                host_metrics=artifact_ref(
                    root, host_metrics, schema="pipetune.host-metrics/v1"
                ),
                failure_reason=None,
            )
            path = root / "trial.json"
            write_trial_manifest(path, manifest)
            loaded = load_trial_manifest(path, artifact_root=root)
            self.assertEqual(loaded, manifest)
            self.assertEqual(sha256_file(path), sha256_file(path))

            document = json.loads(path.read_text())
            unknown = copy.deepcopy(document)
            unknown["unknown"] = True
            write_json_atomic(path, unknown)
            with self.assertRaises(ContractError):
                load_trial_manifest(path, artifact_root=root)

            write_trial_manifest(path, manifest)
            (root / "target" / "stdout.txt").write_text("tampered\n")
            with self.assertRaises(ContractError):
                load_trial_manifest(path, artifact_root=root)

    def test_session_round_trip_indexes_typed_trial_manifests(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-session-") as temp_dir:
            root = pathlib.Path(temp_dir)
            trial_dir = root / "trials" / "trial-0001"
            trial_dir.mkdir(parents=True)
            trial_path = trial_dir / "trial.json"
            trial_path.write_text("{}\n")
            session = SessionManifest(
                schema="pipetune.session/v1",
                session_id="session-0001",
                started_at_utc=START,
                ended_at_utc=END,
                status="complete",
                trials=(
                    artifact_ref(root, trial_path, schema="pipetune.trial/v1"),
                ),
                failure_reason=None,
            )
            path = root / "session.json"
            write_session_manifest(path, session)
            self.assertEqual(
                load_session_manifest(path, artifact_root=root),
                session,
            )
            trial_path.write_text("tampered\n")
            with self.assertRaises(ContractError):
                load_session_manifest(path, artifact_root=root)

    def test_host_metrics_round_trip_rejects_fabricated_zero(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-host-metrics-") as temp_dir:
            root = pathlib.Path(temp_dir)
            raw = root / "perf.stderr"
            raw.write_text("permission denied\n")
            counters = (
                CounterValue("llc_load", True, 5, 100, 5.0, None),
                CounterValue("llc_store", True, 2, 100, 2.0, None),
                CounterValue("io_read", False, None, None, None, "unsupported"),
                CounterValue("io_write", False, None, None, None, "unsupported"),
            )
            sample = MetricSample(
                schema="pipetune.host-metrics/v1",
                sample_id="sample-1",
                endpoint_id="target",
                started_at_utc=START,
                ended_at_utc=END,
                sample_interval_seconds=1.0,
                socket_id=0,
                counters=counters,
                providers=(
                    ProviderStatus(
                        name="perf",
                        available=False,
                        path="/usr/bin/perf",
                        version=None,
                        reason="permission denied",
                    ),
                    ProviderStatus(
                        name="pcm_pcie",
                        available=False,
                        path="/usr/sbin/pcm-pcie",
                        version=None,
                        reason="permission denied",
                    ),
                ),
                commands=(),
                raw_artifacts=(artifact_ref(root, raw),),
            )
            path = root / "host-metrics.json"
            write_metric_sample(path, sample)
            self.assertEqual(
                load_metric_sample(path, artifact_root=root),
                sample,
            )

            document = json.loads(path.read_text())
            document["counters"][2].update(
                {"numerator": 0, "denominator": 0, "rate_percent": 0.0}
            )
            write_json_atomic(path, document)
            with self.assertRaises(ContractError):
                load_metric_sample(path, artifact_root=root)

            write_metric_sample(path, sample)
            raw.write_text("tampered\n")
            with self.assertRaises(ContractError):
                load_metric_sample(path, artifact_root=root)

            write_metric_sample(path, sample)
            document = json.loads(path.read_text())
            document["schema"] = "pipetune.host-metrics/v2"
            write_json_atomic(path, document)
            with self.assertRaises(ContractError):
                load_metric_sample(path, artifact_root=root)

    def test_artifact_paths_are_relative_and_contained(self) -> None:
        with self.assertRaises(ContractError):
            ArtifactRef(path="/tmp/raw", sha256="a" * 64, size_bytes=1)
        with self.assertRaises(ContractError):
            ArtifactRef(path="../raw", sha256="a" * 64, size_bytes=1)
        with self.assertRaises(ContractError):
            ArtifactRef(
                path="raw", sha256="a" * 64, size_bytes=1, schema="axio.metrics/v2"
            )


if __name__ == "__main__":
    unittest.main()
