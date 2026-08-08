from __future__ import annotations

import contextlib
import io
import json
import pathlib
import types
import unittest
from unittest import mock

from pipetune import __main__


class CliTest(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
