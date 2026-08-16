from __future__ import annotations

import contextlib
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

from artifact_eval.configuration import CaseConfiguration, common_overrides, target_overrides
from artifact_eval.harness import ArtifactHarness, BuildRecord, ExperimentCase, HarnessOptions
from artifact_eval.manifest import ManifestError, RunManifest, matrix_fingerprint
from artifact_eval.model import profile_defaults
from artifact_eval.runtime import (
    ArtifactRuntimeError,
    BuildCache,
    EndpointCommands,
    _single_rdma_netdev,
)


class RecordingTransport:
    def __init__(self) -> None:
        self.run_arguments: dict[str, object] | None = None

    def run(self, **arguments: object) -> object:
        self.run_arguments = arguments
        return types.SimpleNamespace(
            status="exited", return_code=0, failure_reason=None
        )

    def get_bytes(self, _source: str) -> bytes:
        return b""


class ScriptedTransport:
    def __init__(self) -> None:
        self.commands: list[tuple[str, ...]] = []
        self.files: dict[str, bytes] = {}

    def run(self, **arguments: object) -> object:
        argv = tuple(arguments["argv"])
        self.commands.append(argv)
        stdout = b""
        if argv == ("git", "rev-parse", "HEAD"):
            stdout = b"a" * 40 + b"\n"
        elif argv and argv[0] == "sha256sum":
            stdout = b"b" * 64 + b"  axio\n"
        self.files[str(arguments["stdout_path"])] = stdout
        self.files[str(arguments["stderr_path"])] = b""
        return types.SimpleNamespace(
            status="exited", return_code=0, failure_reason=None
        )

    def get_bytes(self, source: str) -> bytes:
        return self.files[source]

    def put_bytes(self, destination: str, payload: bytes) -> None:
        self.files[destination] = payload


class ArtifactEvaluationCoreTest(unittest.TestCase):
    @staticmethod
    def _case() -> ExperimentCase:
        return ExperimentCase(
            configuration=CaseConfiguration(
                case_id="dpdk-t-app",
                backend="dpdk",
                handler="t_app",
                c1=16,
                c2=16,
                c3=32,
                warmup_windows=2,
                sample_windows=3,
            ),
            mode="bootstrap",
            tuning_rounds=2,
        )

    def test_command_control_files_stay_below_the_ignored_build_tree(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ae-evidence-") as temp_dir:
            transport = RecordingTransport()
            endpoint = types.SimpleNamespace(
                spec=types.SimpleNamespace(workdir="/srv/axio")
            )
            commands = EndpointCommands(
                endpoint, transport, pathlib.Path(temp_dir)
            )

            commands.capture("true", ("true",))

            self.assertIsNotNone(transport.run_arguments)
            for name in ("state_path", "stdout_path", "stderr_path"):
                path = str(transport.run_arguments[name])
                self.assertTrue(
                    path.startswith("/srv/axio/build-ae/.artifact_eval/control/"),
                    f"{name} escaped the ignored build tree: {path}",
                )

    @mock.patch("artifact_eval.runtime.AxioConfigTool")
    def test_build_cache_rebinds_an_existing_meson_directory_to_the_new_config(
        self, tool_type: mock.Mock
    ) -> None:
        transport = ScriptedTransport()
        tool_type.return_value.fingerprints.return_value = {"build": "build-id"}
        tool_type.return_value.resolve.return_value = types.SimpleNamespace(
            spec=types.SimpleNamespace(workdir="/srv/axio", role="server")
        )
        with tempfile.TemporaryDirectory(prefix="ae-build-cache-") as temp_dir:
            root = pathlib.Path(temp_dir)
            config = root / "server.toml"
            config.write_text("schema_version = 1\n", encoding="utf-8")
            cache = BuildCache(
                configure_binary=root / "axio-configure",
                evidence_root=root / "evidence",
                expected_git_commit="a" * 40,
                transport_factory=lambda _spec: transport,
            )

            cache.ensure("target", config)

        remote_config = "/srv/axio/build-ae/.artifact_eval/build-configs/build-id.toml"
        self.assertIn(
            (
                "meson",
                "configure",
                "/srv/axio/build-ae/build-id",
                f"-Daxio_config={remote_config}",
            ),
            transport.commands,
        )

    def test_rdma_device_resolves_exactly_one_linux_netdev(self) -> None:
        self.assertEqual(_single_rdma_netdev(b"rdma0\n"), "rdma0")
        with self.assertRaises(ArtifactRuntimeError):
            _single_rdma_netdev(b"")
        with self.assertRaises(ArtifactRuntimeError):
            _single_rdma_netdev(b"rdma0\nrdma1\n")

    def test_profiles_publish_reproducible_defaults(self) -> None:
        smoke = profile_defaults("smoke", experiment="figure3")
        paper = profile_defaults("paper", experiment="figure3")
        e2e = profile_defaults("paper", experiment="e2e")

        self.assertEqual((smoke.warmup_windows, smoke.sample_windows), (2, 3))
        self.assertEqual(smoke.repeats, 1)
        self.assertEqual(smoke.tuning_rounds, 3)
        self.assertEqual(paper.repeats, 20)
        self.assertEqual(e2e.tuning_rounds, 20)

    def test_case_overrides_separate_common_build_values_from_target_topology(self) -> None:
        case = CaseConfiguration(
            case_id="m-app-c1-8",
            backend="roce",
            handler="m_app",
            c1=8,
            c2=1,
            c3=16,
            warmup_windows=2,
            sample_windows=3,
            stage_distribution=True,
        )

        common = common_overrides(case)
        target = target_overrides(case)
        self.assertEqual(common["network.backend"], "roce")
        self.assertEqual(common["knobs.build.mempool_handler"], "huge_alloc")
        self.assertNotIn("knobs.build.mtu", common)
        self.assertEqual(common["other.mempool_cache_size"], 0)
        self.assertTrue(common["metrics.stage_distribution.enabled"])
        self.assertEqual(common["other.iterations"], 10)
        self.assertNotIn("knobs.runtime.application_core_count", common)
        self.assertEqual(target["knobs.runtime.application_core_count"], 8)
        self.assertEqual(target["knobs.runtime.dispatcher_queue_count"], 1)
        self.assertEqual(target["knobs.runtime.nic_rx_post_size"], 16)
        self.assertNotIn("other.iterations", target)

    def test_resume_requires_identical_identity_and_complete_case_artifacts(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ae-manifest-") as temp_dir:
            root = pathlib.Path(temp_dir) / "run"
            manifest = RunManifest.create(
                root,
                experiment="figure3",
                profile="smoke",
                git_commit="a" * 40,
                matrix_fingerprint="b" * 64,
                cases=("c1-4", "c1-8"),
                matrix=({"case_id": "c1-4"}, {"case_id": "c1-8"}),
            )
            case_dir = root / "cases/c1-4"
            case_dir.mkdir(parents=True)
            artifact = case_dir / "session.json"
            artifact.write_text("{}\n", encoding="utf-8")
            manifest.complete_case("c1-4", (artifact,))
            summary = root / "summary.md"
            summary.write_text("summary\n", encoding="utf-8")
            manifest.publish_files((summary,))

            resumed = RunManifest.resume(
                root,
                experiment="figure3",
                profile="smoke",
                git_commit="a" * 40,
                matrix_fingerprint="b" * 64,
            )
            self.assertEqual(resumed.completed_cases(), ("c1-4",))
            self.assertIn("summary.md", resumed.document["publications"])

            artifact.write_text("changed\n", encoding="utf-8")
            with self.assertRaises(ManifestError):
                RunManifest.resume(
                    root,
                    experiment="figure3",
                    profile="smoke",
                    git_commit="a" * 40,
                    matrix_fingerprint="b" * 64,
                )

    def test_persisted_experiment_case_reconstructs_the_exact_matrix_entry(self) -> None:
        case = self._case()
        self.assertTrue(
            hasattr(ExperimentCase, "from_document"),
            "ExperimentCase must deserialize its persisted manifest form",
        )

        restored = ExperimentCase.from_document(case.as_document())

        self.assertEqual(restored, case)

    def test_manifest_validates_snapshotted_reference_inputs(self) -> None:
        case = self._case()
        with tempfile.TemporaryDirectory(prefix="ae-inputs-") as temp_dir:
            root = pathlib.Path(temp_dir) / "run"
            manifest = RunManifest.create(
                root,
                experiment="e2e",
                profile="paper",
                git_commit="a" * 40,
                matrix_fingerprint=matrix_fingerprint([case.as_document()]),
                cases=(case.configuration.case_id,),
                matrix=(case.as_document(),),
            )
            reference = root / "inputs/reference-configs/client-dpdk.toml"
            reference.parent.mkdir(parents=True)
            reference.write_text("schema_version = 1\n", encoding="utf-8")
            self.assertTrue(
                hasattr(manifest, "record_inputs"),
                "RunManifest must record immutable input evidence",
            )

            manifest.record_inputs((reference,))
            identity = RunManifest.inspect(root)

            self.assertEqual(identity.experiment, "e2e")
            self.assertEqual(identity.profile, "paper")
            self.assertEqual(identity.matrix, (case.as_document(),))
            RunManifest.resume(
                root,
                experiment="e2e",
                profile="paper",
                git_commit="a" * 40,
                matrix_fingerprint=matrix_fingerprint([case.as_document()]),
            )

            reference.write_text("changed\n", encoding="utf-8")
            with self.assertRaises(ManifestError):
                RunManifest.resume(
                    root,
                    experiment="e2e",
                    profile="paper",
                    git_commit="a" * 40,
                    matrix_fingerprint=matrix_fingerprint([case.as_document()]),
                )

    def test_resume_dry_run_reconstructs_profile_and_case_from_manifest(self) -> None:
        case = self._case()
        repository = pathlib.Path(__file__).resolve().parents[2]
        git_commit = subprocess.run(
            ("git", "rev-parse", "HEAD"),
            cwd=repository,
            check=True,
            stdout=subprocess.PIPE,
            text=True,
        ).stdout.strip()
        with tempfile.TemporaryDirectory(prefix="ae-resume-cli-") as temp_dir:
            root = pathlib.Path(temp_dir) / "run"
            RunManifest.create(
                root,
                experiment="e2e",
                profile="paper",
                git_commit=git_commit,
                matrix_fingerprint=matrix_fingerprint([case.as_document()]),
                cases=(case.configuration.case_id,),
                matrix=(case.as_document(),),
            )

            completed = subprocess.run(
                (
                    sys.executable,
                    "-m",
                    "artifact_eval",
                    "e2e",
                    "--resume",
                    str(root),
                    "--dry-run",
                ),
                cwd=repository,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("Profile: paper", completed.stdout)
            self.assertIn("dpdk-t-app", completed.stdout)
            self.assertNotIn("dpdk-l-app", completed.stdout)

    @mock.patch("artifact_eval.harness.measure")
    @mock.patch("artifact_eval.harness.BuildCache")
    @mock.patch("artifact_eval.harness.Preflight")
    @mock.patch("artifact_eval.harness.ConfigMaterializer")
    @mock.patch("artifact_eval.harness.ensure_configure_binary")
    @mock.patch("artifact_eval.harness._git_commit", return_value="a" * 40)
    def test_harness_reports_major_execution_stages_to_stderr(
        self,
        _commit: mock.Mock,
        configure: mock.Mock,
        materializer_type: mock.Mock,
        preflight_type: mock.Mock,
        build_cache_type: mock.Mock,
        measure_run: mock.Mock,
    ) -> None:
        case = ExperimentCase(
            configuration=self._case().configuration,
            mode="measure",
        )
        build_cache_type.return_value.ensure.side_effect = (
            BuildRecord("target", "server", "target-build", "a" * 64, "/bin/true", "b" * 64, "build-target"),
            BuildRecord("peer", "client", "peer-build", "c" * 64, "/bin/true", "d" * 64, "build-peer"),
        )

        def materialize(**arguments: object) -> None:
            pathlib.Path(arguments["target_output"]).parent.mkdir(parents=True)
            pathlib.Path(arguments["target_output"]).write_text("target\n", encoding="utf-8")
            pathlib.Path(arguments["peer_output"]).write_text("peer\n", encoding="utf-8")

        materializer_type.return_value.materialize.side_effect = materialize

        def publish_measure(request: object) -> None:
            request.output.mkdir(parents=True)
            (request.output / "trial.txt").write_text("complete\n", encoding="utf-8")

        measure_run.side_effect = publish_measure
        configure.return_value = pathlib.Path("/bin/true")
        preflight_type.return_value.check_pair.return_value = {}
        with tempfile.TemporaryDirectory(prefix="ae-progress-") as temp_dir:
            root = pathlib.Path(temp_dir)
            config_dir = root / "configs"
            config_dir.mkdir()
            for role in ("client", "server"):
                (config_dir / f"{role}-dpdk.toml").write_text(
                    f"role = {role!r}\n", encoding="utf-8"
                )
            harness = ArtifactHarness(
                HarnessOptions(
                    repository=root,
                    config_dir=config_dir,
                    output=root / "output",
                    profile="smoke",
                )
            )
            stderr = io.StringIO()

            with contextlib.redirect_stderr(stderr):
                harness.execute("figure3", (case,))

            progress = stderr.getvalue()
            self.assertIn("preflight dpdk", progress)
            self.assertIn("build target", progress)
            self.assertIn("build peer", progress)
            self.assertIn("case 1/1: dpdk-t-app", progress)
            self.assertIn("measure repeat 1/1", progress)


if __name__ == "__main__":
    unittest.main()
