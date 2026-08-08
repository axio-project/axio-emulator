from __future__ import annotations

import dataclasses
import unittest

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
    TrialResult,
)


SHA256 = "a" * 64
GIT_SHA = "b" * 40
START = "2026-08-08T00:00:00+00:00"
END = "2026-08-08T00:00:01+00:00"


def artifact(path: str, schema: str | None = None) -> ArtifactRef:
    return ArtifactRef(path=path, sha256=SHA256, size_bytes=7, schema=schema)


def endpoint(endpoint_id: str, role: str) -> TrialEndpoint:
    spec = EndpointSpec(
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
    )
    fingerprints = FingerprintSet(
        git_commit=GIT_SHA,
        binary_sha256=SHA256,
        source_config_sha256=SHA256,
        build="fnv1a64:0000000000000001",
        datapath="fnv1a64:0000000000000002",
        deployment="fnv1a64:0000000000000003",
    )
    process = ProcessResult(
        argv=("./axio", "--config", "session.toml"),
        status="exited",
        return_code=0,
        failure_reason=None,
        started_at_utc=START,
        ended_at_utc=END,
        stdout=artifact(f"{endpoint_id}/stdout.txt"),
        stderr=artifact(f"{endpoint_id}/stderr.txt"),
    )
    return TrialEndpoint(
        spec=spec,
        fingerprints=fingerprints,
        process=process,
        artifacts=(
            artifact(f"{endpoint_id}/metrics.jsonl", "axio.metrics/v1"),
        ),
    )


class ModelContractTest(unittest.TestCase):
    def test_process_result_distinguishes_transport_failures(self) -> None:
        completed = endpoint("target", "client").process
        disconnected = dataclasses.replace(
            completed,
            status="transport_error",
            return_code=None,
            failure_reason="SSH connection closed",
        )
        self.assertEqual(disconnected.status, "transport_error")
        with self.assertRaises(ContractError):
            dataclasses.replace(completed, status="timed_out", return_code=None)
        with self.assertRaises(ContractError):
            dataclasses.replace(
                completed,
                status="exited",
                failure_reason="fabricated failure",
            )

    def test_endpoint_transport_contract(self) -> None:
        local = endpoint("target", "client").spec
        self.assertEqual(local.transport, "local")
        ssh = dataclasses.replace(
            local,
            transport="ssh",
            host="rDesktop_01",
            ssh_port=22,
            ssh_user="ubuntu",
        )
        self.assertEqual(ssh.host, "rDesktop_01")
        for changes in (
            {"transport": "direct"},
            {"role": "sender"},
            {"backend": "raw"},
            {"numa_node": True},
            {"transport": "ssh", "host": "", "ssh_user": "ubuntu"},
            {"transport": "ssh", "host": "host", "ssh_user": ""},
            {"transport": "ssh", "host": "host", "ssh_user": "u", "ssh_port": 0},
        ):
            with self.subTest(changes=changes), self.assertRaises(ContractError):
                dataclasses.replace(local, **changes)

    def test_counter_availability_is_explicit(self) -> None:
        available = CounterValue(
            name="llc_load",
            available=True,
            numerator=5,
            denominator=100,
            rate_percent=5.0,
            reason=None,
        )
        self.assertEqual(available.rate_percent, 5.0)
        unavailable = CounterValue(
            name="io_read",
            available=False,
            numerator=None,
            denominator=None,
            rate_percent=None,
            reason="permission denied",
        )
        self.assertIsNone(unavailable.rate_percent)
        with self.assertRaises(ContractError):
            dataclasses.replace(unavailable, numerator=0, denominator=0, rate_percent=0.0)
        with self.assertRaises(ContractError):
            dataclasses.replace(available, rate_percent=110.0)

    def test_metric_sample_requires_all_four_counters(self) -> None:
        counters = tuple(
            CounterValue(
                name=name,
                available=False,
                numerator=None,
                denominator=None,
                rate_percent=None,
                reason="not sampled",
            )
            for name in ("llc_load", "llc_store", "io_read", "io_write")
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
            raw_artifacts=(artifact("providers/perf.stderr"),),
        )
        self.assertEqual(len(sample.counters), 4)
        with self.assertRaises(ContractError):
            dataclasses.replace(sample, counters=counters[:-1])
        with self.assertRaises(ContractError):
            dataclasses.replace(sample, providers=sample.providers[:-1])
        with self.assertRaises(ContractError):
            dataclasses.replace(
                sample,
                raw_artifacts=(sample.raw_artifacts[0], sample.raw_artifacts[0]),
            )
        with self.assertRaises(ContractError):
            dataclasses.replace(sample, sample_interval_seconds=0.0)
        with self.assertRaises(ContractError):
            dataclasses.replace(sample, socket_id=True)

    def test_trial_manifest_and_result_are_immutable(self) -> None:
        manifest = TrialManifest(
            schema="pipetune.trial/v1",
            trial_id="trial-0001",
            target_endpoint_id="target",
            started_at_utc=START,
            ended_at_utc=END,
            status="success",
            cleanup_status="clean",
            endpoints=(endpoint("target", "client"), endpoint("peer", "server")),
            host_metrics=artifact(
                "target/host-metrics.json", "pipetune.host-metrics/v1"
            ),
            failure_reason=None,
        )
        result = TrialResult(
            session=artifact("session.json", "pipetune.session/v1"),
            manifest=artifact("trial.json", "pipetune.trial/v1"),
            success=True,
            target_metrics=artifact("target/metrics.jsonl", "axio.metrics/v1"),
            peer_metrics=artifact("peer/metrics.jsonl", "axio.metrics/v1"),
            host_metrics=artifact(
                "target/host-metrics.json", "pipetune.host-metrics/v1"
            ),
            failure_reason=None,
        )
        self.assertTrue(result.success)
        session = SessionManifest(
            schema="pipetune.session/v1",
            session_id="session-0001",
            started_at_utc=START,
            ended_at_utc=END,
            status="complete",
            trials=(artifact("trials/trial-0001/trial.json", "pipetune.trial/v1"),),
            failure_reason=None,
        )
        self.assertEqual(session.trials[0].schema, "pipetune.trial/v1")
        with self.assertRaises(dataclasses.FrozenInstanceError):
            manifest.status = "failed"  # type: ignore[misc]
        with self.assertRaises(ContractError):
            dataclasses.replace(manifest, target_endpoint_id="missing")
        with self.assertRaises(ContractError):
            dataclasses.replace(manifest, endpoints=(endpoint("target", "client"),))
        with self.assertRaises(ContractError):
            dataclasses.replace(session, trials=())


if __name__ == "__main__":
    unittest.main()
