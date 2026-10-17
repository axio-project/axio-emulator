"""Regression evidence for configured effect thresholds, not window dispersion."""

import dataclasses
import unittest

from pipetune.diagnosis import statistic_from_samples
from pipetune.objective import ObjectivePolicy, ObjectiveTrial, compare_candidate


POLICY = ObjectivePolicy(100.0, 0.03, 0.01)


def trial(name, latency, throughput):
    return ObjectiveTrial(
        name, "valid",
        statistic_from_samples(tuple(latency), relative_floor=0.03, unit="us"),
        statistic_from_samples(tuple(throughput), relative_floor=0.01, unit="Mpps"),
        None,
    )


class ConfiguredThresholdTest(unittest.TestCase):
    def test_roce_lapp_feasible_candidate_survives_baseline_variation(self):
        baseline = trial("baseline", [
            268.626, 93.196, 81.107, 10.479, 88.018, 214.625, 212.767,
            113.356, 128.911, 6.158, 72.05, 92.708, 190.411, 275.605,
            117.609, 5.564, 6.271, 152.273, 149.177, 7.462,
        ], [35.3435])
        candidate = trial("candidate", [
            18.147, 6.269, 7.942, 94.189, 6.953, 5.698, 6.514, 6.202,
            7.505, 11.393, 7.0, 29.003, 5.965, 7.333, 7.497, 6.767,
            7.052, 37.685, 48.068, 7.699,
        ], [41.2125])
        comparison = compare_candidate(baseline, candidate, POLICY)
        self.assertTrue(comparison.accepted)
        self.assertTrue(comparison.candidate_feasible)
        self.assertAlmostEqual(comparison.required_improvement, 3.09828)

    def test_variable_baseline_cannot_widen_resource_loss_tolerance(self):
        comparison = compare_candidate(
            trial("baseline", [10.0], [38.0, 40.0, 42.0]),
            trial("candidate", [10.0], [35.0]), POLICY,
            allow_equivalent_resource_reduction=True,
            accepted_physical_cores=16, candidate_physical_cores=15,
        )
        self.assertFalse(comparison.accepted)
        self.assertAlmostEqual(comparison.required_improvement, 0.4)

    def test_statistic_persists_configured_bound_not_dispersion(self):
        value = statistic_from_samples((1.0, 100.0, 199.0), relative_floor=0.05, unit="us")
        document = dataclasses.asdict(value)
        self.assertNotIn("mad", document)
        self.assertNotIn("uncertainty", document)
        self.assertEqual(document["threshold"], 5.0)
