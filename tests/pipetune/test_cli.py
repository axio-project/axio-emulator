from __future__ import annotations

import contextlib
import io
import pathlib
import types
import unittest
from unittest import mock

from pipetune import __main__


class CliTest(unittest.TestCase):
    def test_measure_cli_maps_controller_paths_and_binary_overrides(self) -> None:
        result = types.SimpleNamespace(
            success=True,
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
            '{\n  "manifest": "trial.json",\n  "success": true\n}\n',
        )


if __name__ == "__main__":
    unittest.main()
