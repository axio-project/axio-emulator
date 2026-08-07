from __future__ import annotations

import copy
import json
import pathlib
import tempfile
import unittest

from pipetune.metrics import AXIO_METRICS_SCHEMA, load_axio_jsonl
from pipetune.model import ContractError


ROOT = pathlib.Path(__file__).resolve().parents[2]
VALID_FIXTURE = ROOT / "tests/metrics/fixtures/valid-window.jsonl"
NULL_FIXTURE = ROOT / "tests/metrics/fixtures/null-nic-window.jsonl"


class MetricsContractTest(unittest.TestCase):
    def _valid_document(self) -> dict[str, object]:
        return json.loads(VALID_FIXTURE.read_text())

    def _load_documents(self, documents: list[dict[str, object]]) -> None:
        with tempfile.TemporaryDirectory(prefix="pipetune-metrics-") as temp_dir:
            path = pathlib.Path(temp_dir) / "metrics.jsonl"
            path.write_text(
                "".join(json.dumps(document) + "\n" for document in documents)
            )
            load_axio_jsonl(path, schema=AXIO_METRICS_SCHEMA)

    def test_loads_exact_six_stage_contract(self) -> None:
        windows = load_axio_jsonl(VALID_FIXTURE, schema=AXIO_METRICS_SCHEMA)
        self.assertEqual(len(windows), 1)
        self.assertEqual(windows[0].window_id, 0)
        self.assertEqual(windows[0].stage_names, (
            "app_tx", "app_rx", "dispatcher_tx", "dispatcher_rx",
            "nic_tx", "nic_rx",
        ))
        self.assertAlmostEqual(windows[0].nic_rx.completion_interval_ns or 0, 35.33)
        null_windows = load_axio_jsonl(NULL_FIXTURE, schema=AXIO_METRICS_SCHEMA)
        self.assertIsNone(null_windows[0].nic_rx.completion_interval_ns)

    def test_rejects_structural_and_numeric_mutations(self) -> None:
        valid = self._valid_document()
        mutations: list[tuple[str, dict[str, object]]] = []
        missing = copy.deepcopy(valid)
        del missing["throughput"]
        mutations.append(("missing", missing))
        unknown = copy.deepcopy(valid)
        unknown["identity"] = {"role": "client"}
        mutations.append(("unknown", unknown))
        boolean = copy.deepcopy(valid)
        boolean["window_id"] = True
        mutations.append(("boolean", boolean))
        negative = copy.deepcopy(valid)
        negative["throughput"]["e2e_mpps"] = -1  # type: ignore[index]
        mutations.append(("negative", negative))
        partial_nic = copy.deepcopy(valid)
        partial_nic["stages"]["nic_rx"]["completion_interval_ns"] = None  # type: ignore[index]
        mutations.append(("partial-nic", partial_nic))
        for name, mutation in mutations:
            with self.subTest(name=name), self.assertRaises(ContractError):
                self._load_documents([mutation])

    def test_rejects_invalid_jsonl_stream(self) -> None:
        valid = self._valid_document()
        duplicate = copy.deepcopy(valid)
        with self.assertRaises(ContractError):
            self._load_documents([valid, duplicate])
        later = copy.deepcopy(valid)
        later["window_id"] = 2
        earlier = copy.deepcopy(valid)
        earlier["window_id"] = 1
        with self.assertRaises(ContractError):
            self._load_documents([later, earlier])
        with tempfile.TemporaryDirectory(prefix="pipetune-metrics-") as temp_dir:
            path = pathlib.Path(temp_dir) / "metrics.jsonl"
            path.write_text(VALID_FIXTURE.read_text() + "\n")
            with self.assertRaises(ContractError):
                load_axio_jsonl(path, schema=AXIO_METRICS_SCHEMA)
            path.write_text('{"window_id":0,"window_id":1}\n')
            with self.assertRaises(ContractError):
                load_axio_jsonl(path, schema=AXIO_METRICS_SCHEMA)
            path.write_text('{"window_id":NaN}\n')
            with self.assertRaises(ContractError):
                load_axio_jsonl(path, schema=AXIO_METRICS_SCHEMA)

    def test_rejects_unknown_external_schema(self) -> None:
        with self.assertRaises(ContractError):
            load_axio_jsonl(VALID_FIXTURE, schema="axio.metrics/v2")


if __name__ == "__main__":
    unittest.main()
