from __future__ import annotations

import contextlib
import io
import json
import pathlib
import tempfile
import types
import unittest
from unittest import mock

from pipetune import __main__
from tests.pipetune.test_diagnosis import build_session


class CliTest(unittest.TestCase):
    def test_paired_reduction_summary_names_the_exact_transition(self) -> None:
        publication = types.SimpleNamespace(
            path=pathlib.Path("session/diagnoses/trial-0001.json"),
            document={
                "result": {
                    "point": "paired_reduction_required",
                    "direction": "rx",
                    "confidence": "none",
                    "confidence_reasons": [
                        "fully colocated completion requires paired reduction"
                    ],
                    "required_probe": None,
                    "required_paired_reduction": {
                        "baseline_application_count": 16,
                        "baseline_dispatcher_count": 16,
                        "candidate_application_count": 15,
                        "candidate_dispatcher_count": 15,
                    },
                },
                "steady_state": {
                    "target": {
                        "throughput": {"median": 43.22},
                        "stage_ranking": [
                            {
                                "name": "app_rx.completion",
                                "statistic": {
                                    "median": 0.12,
                                    "unit": "us/packet",
                                },
                            }
                        ],
                    },
                    "peer": {"throughput": {"median": 43.10}},
                },
                "counter_rates": {
                    "baseline": {
                        "llc_load": {"median": 82.42},
                        "llc_store": {"median": 84.31},
                        "io_read": {"median": 2.40},
                        "io_write": {"median": 90.63},
                    }
                },
            },
        )

        summary = __main__._diagnosis_summary(publication)

        self.assertIn(
            "Next: evaluate paired colocated reduction A16/D16 -> A15/D15",
            summary,
        )

    def test_peer_unhealthy_summary_requires_a_new_measurement(self) -> None:
        publication = types.SimpleNamespace(
            path=pathlib.Path("session/diagnoses/trial-0001.json"),
            document={
                "result": {
                    "point": "peer_unhealthy",
                    "direction": None,
                    "confidence": "none",
                    "confidence_reasons": ["drop: target dispatcher enqueue"],
                    "required_probe": None,
                },
                "steady_state": {
                    "target": {
                        "throughput": {"median": 45.94},
                        "stage_ranking": [
                            {
                                "name": "app_rx.completion",
                                "statistic": {
                                    "median": 0.12,
                                    "unit": "us/packet",
                                },
                            }
                        ],
                    },
                    "peer": {"throughput": {"median": 45.33}},
                },
                "counter_rates": {
                    "baseline": {
                        "llc_load": {"median": 81.22},
                        "llc_store": {"median": 84.28},
                        "io_read": None,
                        "io_write": None,
                    }
                },
            },
        )

        summary = __main__._diagnosis_summary(publication)

        self.assertIn(
            "Next: resolve the health issue and run measure again",
            summary,
        )
        self.assertNotIn("run bootstrap", summary)

    def test_bootstrap_cli_maps_session_inputs(self) -> None:
        published = types.SimpleNamespace(
            document={"schema": "pipetune.status/v1", "phase": "complete"}
        )
        stdout = io.StringIO()
        with (
            mock.patch.object(
                __main__, "bootstrap_session", return_value=published
            ) as bootstrap,
            contextlib.redirect_stdout(stdout),
        ):
            return_code = __main__.main(
                [
                    "bootstrap",
                    "--target-config",
                    "target.toml",
                    "--peer-config",
                    "peer.toml",
                    "--max-iterations",
                    "4",
                    "--output",
                    "results/tune",
                    "--axio-configure",
                    "build-tools/axio-configure",
                    "--target-binary",
                    "/opt/target/axio",
                ]
            )
        request = bootstrap.call_args.args[0]
        self.assertEqual(request.target_config, pathlib.Path("target.toml"))
        self.assertEqual(request.peer_config, pathlib.Path("peer.toml"))
        self.assertEqual(request.max_iterations, 4)
        self.assertEqual(request.output, pathlib.Path("results/tune"))
        self.assertEqual(request.target_binary, "/opt/target/axio")
        self.assertEqual(return_code, 0)
        self.assertEqual(json.loads(stdout.getvalue()), published.document)

    def test_resume_and_status_cli_have_separate_mutation_contracts(self) -> None:
        published = types.SimpleNamespace(
            document={"schema": "pipetune.status/v1", "phase": "complete"}
        )
        for command, function_name in (
            ("resume", "resume_session"),
            ("status", "read_session_status"),
        ):
            with self.subTest(command=command):
                stdout = io.StringIO()
                with (
                    mock.patch.object(
                        __main__, function_name, return_value=published
                    ) as function,
                    contextlib.redirect_stdout(stdout),
                ):
                    arguments = [command, "--session", "results/tune"]
                    if command == "resume":
                        arguments.extend(
                            ["--axio-configure", "build-tools/axio-configure"]
                        )
                    return_code = __main__.main(arguments)
                if command == "resume":
                    request = function.call_args.args[0]
                    self.assertEqual(request.session, pathlib.Path("results/tune"))
                else:
                    self.assertEqual(
                        function.call_args.args[0], pathlib.Path("results/tune")
                    )
                self.assertEqual(return_code, 0)
                self.assertEqual(json.loads(stdout.getvalue()), published.document)

    def test_measure_cli_maps_controller_paths_and_binary_overrides(self) -> None:
        result = types.SimpleNamespace(
            success=True,
            session=types.SimpleNamespace(path="session.json"),
            manifest=types.SimpleNamespace(path="trial.json"),
        )
        stdout = io.StringIO()
        with (
            mock.patch.object(__main__, "measure", return_value=result) as measure,
            contextlib.redirect_stdout(stdout),
        ):
            return_code = __main__.main(
                [
                    "measure",
                    "--target-config",
                    "target.toml",
                    "--peer-config",
                    "peer.toml",
                    "--output",
                    "results/trial",
                    "--axio-configure",
                    "build-tools/axio-configure",
                    "--target-binary",
                    "/opt/axio/build-client/axio",
                    "--peer-binary",
                    "/opt/axio/build-server/axio",
                ]
            )
        request = measure.call_args.args[0]
        self.assertEqual(request.target_config, pathlib.Path("target.toml"))
        self.assertEqual(request.peer_config, pathlib.Path("peer.toml"))
        self.assertEqual(request.output, pathlib.Path("results/trial"))
        self.assertEqual(request.target_binary, "/opt/axio/build-client/axio")
        self.assertEqual(return_code, 0)
        self.assertEqual(
            stdout.getvalue(),
            '{\n  "manifest": "trial.json",\n  "session": "session.json",\n'
            '  "success": true\n}\n',
        )

    def test_diagnose_cli_is_offline_and_prints_published_document(self) -> None:
        published = types.SimpleNamespace(
            path=pathlib.Path("session/diagnoses/trial-0001.json"),
            document={"schema": "pipetune.diagnosis/v1", "result": {"point": "P1"}},
        )
        stdout = io.StringIO()
        with (
            mock.patch.object(
                __main__, "publish_diagnosis", return_value=published
            ) as publish,
            mock.patch.object(__main__, "measure") as measure,
            contextlib.redirect_stdout(stdout),
        ):
            return_code = __main__.main(
                [
                    "diagnose",
                    "--session",
                    "session",
                    "--trial",
                    "trial-0001",
                    "--probe-session",
                    "probe-session",
                    "--probe-trial",
                    "trial-0002",
                    "--json",
                ]
            )
        publish.assert_called_once_with(
            pathlib.Path("session"),
            trial_id="trial-0001",
            probe_session_root=pathlib.Path("probe-session"),
            probe_trial_id="trial-0002",
        )
        measure.assert_not_called()
        self.assertEqual(return_code, 0)
        self.assertEqual(json.loads(stdout.getvalue()), published.document)

    def test_diagnose_cli_summarizes_a_real_measurement_artifact(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-cli-") as temp_dir:
            session = build_session(pathlib.Path(temp_dir))
            stdout = io.StringIO()

            with contextlib.redirect_stdout(stdout):
                return_code = __main__.main(
                    ["diagnose", "--session", str(session)]
                )

            self.assertEqual(return_code, 0)
            self.assertIn("PipeTune diagnosis", stdout.getvalue())
            self.assertIn("Longest stage:", stdout.getvalue())

    def test_diagnose_cli_rejects_an_incompatible_publication_without_traceback(
        self,
    ) -> None:
        publication = types.SimpleNamespace(
            path=pathlib.Path("session/diagnoses/legacy.json"),
            document={"result": {}},
        )
        stderr = io.StringIO()
        with (
            mock.patch.object(
                __main__, "publish_diagnosis", return_value=publication
            ),
            contextlib.redirect_stderr(stderr),
        ):
            try:
                return_code = __main__.main(
                    ["diagnose", "--session", "session"]
                )
            except (KeyError, IndexError, TypeError, ValueError) as error:
                self.fail(f"diagnose propagated {type(error).__name__}: {error}")

        self.assertEqual(return_code, 2)
        self.assertIn("invalid diagnosis publication", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
