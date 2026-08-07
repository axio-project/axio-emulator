#!/usr/bin/env python3

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
CLI = SCRIPT_DIR.parents[1] / "toolchain" / "axio_metrics.py"


class AxioMetricsCliTest(unittest.TestCase):
    def run_cli(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(CLI), *arguments],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_pretty_prints_each_jsonl_record(self) -> None:
        source = (
            '{"window_id":0,"throughput":{"e2e_mpps":28.30}}\n'
            '{"window_id":1,"throughput":{"e2e_mpps":29.25}}\n'
        )
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "metrics.jsonl"
            path.write_text(source, encoding="utf-8")
            result = self.run_cli("pretty", str(path))

        expected = """{
  "window_id": 0,
  "throughput": {
    "e2e_mpps": 28.30
  }
}
{
  "window_id": 1,
  "throughput": {
    "e2e_mpps": 29.25
  }
}
"""
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, expected)
        self.assertEqual(result.stderr, "")

    def test_pretty_array_emits_one_standard_json_document(self) -> None:
        records = [{"window_id": 0}, {"window_id": 1}]
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "metrics.jsonl"
            path.write_text(
                "".join(json.dumps(record) + "\n" for record in records),
                encoding="utf-8",
            )
            result = self.run_cli("pretty", "--array", str(path))

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), records)
        self.assertTrue(result.stdout.startswith("[\n"))
        self.assertTrue(result.stdout.endswith("\n"))

    def test_pretty_reports_the_bad_jsonl_line(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "metrics.jsonl"
            path.write_text('{"window_id":0}\nnot-json\n', encoding="utf-8")
            result = self.run_cli("pretty", str(path))

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn("metrics.jsonl:2: invalid JSON", result.stderr)


if __name__ == "__main__":
    unittest.main()
