from __future__ import annotations

import dataclasses
import hashlib
import pathlib
import tempfile
import unittest

from pipetune.application import (
    ApplicationError,
    BootstrapRequest,
    ResumeRequest,
    _initialize,
    _tuning_summary,
    resume_session,
)
from pipetune.controller import ConvergenceResult
from pipetune.model import EndpointSpec
from pipetune.remote import CommandOutcome, ResolvedEndpoint
from pipetune.reporting import publish_session_outputs, status_document
from pipetune.session import TuningSessionStore
from tests.pipetune.test_session import create_store, tree_snapshot


def _document(role: str, transport: str) -> dict[str, object]:
    return {
        "deployment": {
            "role": role,
            "transport": transport,
        },
        "network": {"backend": "dpdk"},
        "tuning": {
            "infrastructure_failure_limit": 2,
            "latency_slo_us": 100.0,
            "max_iterations": 20,
            "noise": {
                "latency_relative_floor": 0.03,
                "throughput_relative_floor": 0.01,
            },
        },
    }


class FakeConfigTool:
    def __init__(self, target_transport: str) -> None:
        self.target_transport = target_transport

    def validate_pair(self, target: pathlib.Path, peer: pathlib.Path) -> None:
        self.dump(target)
        self.dump(peer)

    def dump(self, path: pathlib.Path) -> dict[str, object]:
        is_target = path.name == "target.toml"
        return _document(
            "server" if is_target else "client",
            self.target_transport if is_target else "ssh",
        )

    def resolve(
        self,
        *,
        endpoint_id: str,
        config_path: pathlib.Path,
        binary_override: str | None,
    ) -> ResolvedEndpoint:
        role = "server" if endpoint_id == "target" else "client"
        transport = self.target_transport if endpoint_id == "target" else "ssh"
        spec = EndpointSpec(
            endpoint_id=endpoint_id,
            role=role,
            backend="dpdk",
            transport=transport,
            host="" if transport == "local" else f"{endpoint_id}.example",
            ssh_port=0 if transport == "local" else 22,
            ssh_user="" if transport == "local" else "ubuntu",
            workdir=f"/srv/{endpoint_id}",
            use_sudo=True,
            numa_node=1,
        )
        return ResolvedEndpoint(
            spec=spec,
            config_path=config_path,
            binary_path=binary_override or f"/srv/{endpoint_id}/build-{role}/axio",
        )

    def fingerprints(self, path: pathlib.Path) -> dict[str, str]:
        endpoint = "target" if path.name == "target.toml" else "peer"
        return {
            "build": f"build-{endpoint}",
            "datapath": f"datapath-{endpoint}",
            "deployment": f"deployment-{endpoint}",
        }


class FakeTransport:
    def __init__(self, calls: list[tuple[str, ...]]) -> None:
        self.calls = calls
        self.files: dict[str, bytes] = {}

    def run(self, **kwargs: object) -> CommandOutcome:
        argv = tuple(kwargs["argv"])
        self.calls.append(argv)
        stdout = str(kwargs["stdout_path"])
        self.files[stdout] = (
            ("b" * 40 + "\n").encode()
            if argv[0] == "git"
            else ("a" * 64 + "  axio\n").encode()
        )
        return CommandOutcome(
            argv=argv,
            status="exited",
            return_code=0,
            failure_reason=None,
            started_at_utc="2026-01-01T00:00:00+00:00",
            ended_at_utc="2026-01-01T00:00:01+00:00",
        )

    def get_bytes(self, source: str) -> bytes:
        return self.files[source]

    def remove_tree(self, path: str, *, containment_root: str) -> None:
        del path, containment_root


class ApplicationTest(unittest.TestCase):
    def test_tuning_summary_uses_historical_best_objective_and_active_counts(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-summary-") as temp_dir:
            root = pathlib.Path(temp_dir)
            store, _identity, accepted, state = create_store(root)
            statistic = {
                "samples": [44.435],
                "median": 44.435,
                "mad": 0.26,
                "uncertainty": 0.78,
                "unit": "Mpps",
            }
            p999 = {
                "samples": [47.565],
                "median": 47.565,
                "mad": 2.18,
                "uncertainty": 6.54,
                "unit": "us",
            }
            state = store.transition(
                state,
                phase="complete",
                details={
                    **state.details,
                    "convergence": {
                        "best": {
                            "target": dataclasses.asdict(accepted.target),
                            "peer": dataclasses.asdict(accepted.peer),
                        },
                        "best_objective": {
                            "trial_id": "trial-best",
                            "status": "valid",
                            "client_p999": p999,
                            "server_throughput": statistic,
                            "rejection_reason": None,
                        },
                        "best_trial_id": "trial-best",
                        "completed_rounds": 2,
                        "infrastructure_failures": 0,
                        "stop_reason": "max_iterations",
                    },
                },
            )
            result = ConvergenceResult(
                state=state,
                rounds=(),
                stop_reason="max_iterations",
                completed_rounds=2,
                infrastructure_failures=0,
                best=accepted,
                best_trial_id="trial-best",
            )

            class SummaryConfigTool:
                @staticmethod
                def dump(_path: pathlib.Path) -> dict[str, object]:
                    return {
                        "knobs": {
                            "runtime": {
                                "application_core_count": 15,
                                "dispatcher_queue_count": 15,
                                "app_rx_batch_size": 64,
                                "app_tx_batch_size": 32,
                                "dispatcher_rx_batch_size": 32,
                                "dispatcher_tx_batch_size": 32,
                                "nic_rx_post_size": 128,
                                "nic_tx_post_size": 32,
                            }
                        }
                    }

            summary = _tuning_summary(
                root,
                store,
                result,
                SummaryConfigTool(),
            )

            self.assertEqual(summary.target_throughput_mpps, 44.435)
            self.assertEqual(summary.client_p999_us, 47.565)
            self.assertEqual(summary.application_core_count, 15)
            self.assertEqual(summary.dispatcher_queue_count, 15)
            self.assertEqual(summary.report_path, root / "report.md")

    def test_identity_bootstrap_is_placement_neutral_and_does_not_run_axio(self) -> None:
        logical_statuses: list[dict[str, object]] = []
        reports: list[bytes] = []
        for target_transport in ("local", "ssh"):
            with self.subTest(target_transport=target_transport), tempfile.TemporaryDirectory(
                prefix="pipetune-bootstrap-"
            ) as temp_dir:
                root = pathlib.Path(temp_dir)
                target = root / "target.toml"
                peer = root / "peer.toml"
                configure = root / "axio-configure"
                target.write_text("target\n", encoding="utf-8")
                peer.write_text("peer\n", encoding="utf-8")
                configure.write_bytes(b"config tool")
                calls: list[tuple[str, ...]] = []
                transports: list[str] = []

                def transport_factory(spec: EndpointSpec) -> FakeTransport:
                    transports.append(spec.transport)
                    return FakeTransport(calls)

                output = _initialize(
                    BootstrapRequest(
                        target_config=target,
                        peer_config=peer,
                        output=root / "session",
                        configure_binary=configure,
                        max_iterations=4,
                    ),
                    config_tool=FakeConfigTool(target_transport),
                    transport_factory=transport_factory,
                    session_id="tuning-test",
                )
                store = TuningSessionStore(output)
                state = store.status()
                self.assertEqual(state.phase, "baseline")
                self.assertEqual(state.details["bootstrap"]["max_iterations"], 4)
                self.assertEqual(transports, [target_transport, "ssh"])
                self.assertEqual(len(calls), 4)
                self.assertTrue(all(argv[0] in ("git", "sha256sum") for argv in calls))
                complete = store.transition(
                    state,
                    phase="complete",
                    details={
                        **state.details,
                        "convergence": {
                            "best": {
                                "target": dataclasses.asdict(state.accepted.target),
                                "peer": dataclasses.asdict(state.accepted.peer),
                            },
                            "best_objective": None,
                            "best_trial_id": None,
                            "completed_rounds": 0,
                            "infrastructure_failures": 0,
                            "stop_reason": "max_iterations",
                        },
                    },
                )
                result = ConvergenceResult(
                    state=complete,
                    rounds=(),
                    stop_reason="max_iterations",
                    completed_rounds=0,
                    infrastructure_failures=0,
                    best=state.accepted,
                    best_trial_id=None,
                )
                publish_session_outputs(output, store, result)
                logical_statuses.append(status_document(output, store))
                reports.append((output / "report.md").read_bytes())
        self.assertEqual(logical_statuses[0], logical_statuses[1])
        self.assertEqual(reports[0], reports[1])

    def test_complete_resume_is_idempotent_and_does_not_probe_endpoints(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-resume-") as temp_dir:
            root = pathlib.Path(temp_dir)
            configure = root / "axio-configure"
            configure.write_bytes(b"config tool")
            configure_sha = hashlib.sha256(configure.read_bytes()).hexdigest()
            store, _, accepted, state = create_store(
                root,
                details={
                    "round": 0,
                    "bootstrap": {
                        "configure_sha256": configure_sha,
                        "infrastructure_failure_limit": 2,
                        "max_iterations": 1,
                        "objective": {
                            "latency_relative_floor": 0.03,
                            "latency_slo_us": 100.0,
                            "throughput_relative_floor": 0.01,
                        },
                        "peer_binary": None,
                        "schema": "pipetune.bootstrap/v1",
                        "target_binary": None,
                    },
                },
            )
            state = store.transition(
                state,
                phase="complete",
                details={
                    **state.details,
                    "convergence": {
                        "best": {
                            "target": dataclasses.asdict(accepted.target),
                            "peer": dataclasses.asdict(accepted.peer),
                        },
                        "best_objective": None,
                        "best_trial_id": None,
                        "completed_rounds": 0,
                        "infrastructure_failures": 0,
                        "stop_reason": "max_iterations",
                    },
                },
            )
            def forbidden_transport(_spec: EndpointSpec) -> FakeTransport:
                raise AssertionError("complete resume must not probe endpoints")

            first = resume_session(
                ResumeRequest(session=root, configure_binary=configure),
                transport_factory=forbidden_transport,
            )
            before = tree_snapshot(root)

            second = resume_session(
                ResumeRequest(session=root, configure_binary=configure),
                transport_factory=forbidden_transport,
            )

            self.assertEqual(first.document, second.document)
            self.assertEqual(tree_snapshot(root), before)
            self.assertEqual(second.document["phase"], "complete")

            changed = root / "different-axio-configure"
            changed.write_bytes(b"different config tool")
            before_rejection = tree_snapshot(root)
            with self.assertRaisesRegex(
                ApplicationError, "axio-configure identity changed"
            ):
                resume_session(
                    ResumeRequest(session=root, configure_binary=changed),
                    transport_factory=forbidden_transport,
                )
            self.assertEqual(tree_snapshot(root), before_rejection)


if __name__ == "__main__":
    unittest.main()
