from __future__ import annotations

import unittest
from unittest import mock
import pathlib

from artifact_eval.summary import (
    SummaryError,
    _probe_detail_row,
    _summarize_stage,
    _window_throughput,
)
from pipetune.metrics import (
    AxioWindow,
    NicRxStage,
    NicTxStage,
    PipelineStage,
    WindowCounters,
)
from pipetune.stage_distribution import DistributionSummary


def window(*, throughput: float | None = 1.0, drops: int = 0, errors: int = 0) -> AxioWindow:
    pipeline = PipelineStage(0.1, 0.0)
    return AxioWindow(
        window_id=1,
        e2e_throughput_mpps=throughput,
        latency_p50_us=None,
        latency_p99_us=None,
        latency_p999_us=None,
        app_tx=pipeline,
        app_rx=pipeline,
        dispatcher_tx=pipeline,
        dispatcher_rx=pipeline,
        nic_tx=NicTxStage(throughput, 0.1),
        nic_rx=NicRxStage(throughput, 1.0, 1.0, 1.0, 1.0),
        counters=WindowCounters(drops, 0, errors),
    )


def distribution(sample_count: int) -> DistributionSummary:
    value = 1.0 if sample_count else None
    return DistributionSummary(value, value, value, value, value, value, sample_count)


class ArtifactSummaryTest(unittest.TestCase):
    def test_uses_the_canonical_throughput_field(self) -> None:
        self.assertEqual(_window_throughput(window(throughput=12.5), "window"), 12.5)

    def test_rejects_unusable_windows(self) -> None:
        for candidate in (
            window(throughput=None),
            window(throughput=0.0),
            window(drops=1),
            window(errors=1),
        ):
            with self.subTest(candidate=candidate), self.assertRaises(SummaryError):
                _window_throughput(candidate, "window")

    def test_rejects_a_missing_required_distribution_window(self) -> None:
        with self.assertRaises(SummaryError):
            _summarize_stage([distribution(16), distribution(0)])

    @mock.patch("artifact_eval.summary.summarize_trial")
    @mock.patch("artifact_eval.summary._trial_values", return_value=(10.0, 3.0, 5.0))
    def test_publishes_a_diagnostic_probe_as_a_detail_row(
        self, _values: mock.Mock, summarize: mock.Mock
    ) -> None:
        summarize.return_value.canonical_target = {
            "knobs": {
                "runtime": {
                    "application_core_count": 15,
                    "dispatcher_queue_count": 8,
                    "app_rx_batch_size": 16,
                    "app_tx_batch_size": 16,
                    "dispatcher_rx_batch_size": 16,
                    "dispatcher_tx_batch_size": 16,
                    "nic_rx_post_size": 16,
                    "nic_tx_post_size": 16,
                }
            }
        }
        row = _probe_detail_row(
            {"round": 1, "probe": {"trial_id": "probe-1"}},
            session=pathlib.Path("session"),
            diagnosis_text="P2/rx",
            baseline_id="baseline-1",
            candidate_trial_ids=set(),
        )

        self.assertIsNotNone(row)
        self.assertEqual(row["Action"], "c1-probe")
        self.assertEqual(row["Decision"], "diagnostic-only")
        self.assertEqual(row["C1/C2/C3"], "15/8/16")


if __name__ == "__main__":
    unittest.main()
