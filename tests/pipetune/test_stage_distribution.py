from __future__ import annotations

import pathlib
import tempfile
import unittest

from pipetune.model import ContractError
from pipetune.stage_distribution import load_stage_distribution_jsonl


VALID = (
    '{"schema":"axio.stage-distribution/v1","window_id":2,'
    '"sample_stride":64,"stages":{'
    '"app_tx_allocation_stall":{"unit":"us_per_batch","p1_us":0.04,'
    '"p50_us":0.05,"p99_us":0.10,"mean_us":0.06,"min_us":0.03,'
    '"max_us":0.20,"sample_count":100},'
    '"app_rx_handler_completion":{"unit":"us_per_batch","p1_us":0.08,'
    '"p50_us":0.10,"p99_us":0.15,"mean_us":0.11,"min_us":0.07,'
    '"max_us":0.30,"sample_count":100}}}\n'
)


class StageDistributionTest(unittest.TestCase):
    def test_loads_complete_records(self) -> None:
        with tempfile.TemporaryDirectory(prefix="stage-distribution-") as temp_dir:
            path = pathlib.Path(temp_dir) / "distribution.jsonl"
            path.write_text(VALID, encoding="utf-8")
            records = load_stage_distribution_jsonl(path)

        self.assertEqual(len(records), 1)
        self.assertEqual(records[0].window_id, 2)
        self.assertEqual(records[0].app_rx_handler_completion.p99_us, 0.15)

    def test_rejects_truncated_or_unavailable_required_distribution(self) -> None:
        cases = (
            VALID.rstrip("\n"),
            VALID.replace('"p99_us":0.10', '"p99_us":null'),
            VALID.replace('"sample_count":100', '"sample_count":0', 1),
        )
        for payload in cases:
            with self.subTest(payload=payload), tempfile.TemporaryDirectory(
                prefix="stage-distribution-"
            ) as temp_dir:
                path = pathlib.Path(temp_dir) / "distribution.jsonl"
                path.write_text(payload, encoding="utf-8")
                with self.assertRaises(ContractError):
                    load_stage_distribution_jsonl(path)


if __name__ == "__main__":
    unittest.main()
