from __future__ import annotations

import types
import unittest

from pipetune.diagnosis import Statistic
from pipetune.objective import (
    ObjectivePolicy,
    ObjectiveTrial,
    compare_candidate,
    objective_trial_from_summary,
    select_historical_best,
)


def statistic(value: float, *, mad: float = 0.0, unit: str) -> Statistic:
    return Statistic(
        samples=(value,),
        median=value,
        mad=mad,
        uncertainty=max(3.0 * mad, value * 0.01),
        unit=unit,
    )


def trial(
    trial_id: str,
    *,
    latency_us: float,
    throughput_mpps: float,
    latency_mad: float = 0.0,
    throughput_mad: float = 0.0,
) -> ObjectiveTrial:
    return ObjectiveTrial(
        trial_id=trial_id,
        status="valid",
        client_p999=statistic(latency_us, mad=latency_mad, unit="us"),
        server_throughput=statistic(
            throughput_mpps, mad=throughput_mad, unit="Mpps"
        ),
        rejection_reason=None,
    )


def rejected(trial_id: str, status: str) -> ObjectiveTrial:
    return ObjectiveTrial(
        trial_id=trial_id,
        status=status,
        client_p999=None,
        server_throughput=None,
        rejection_reason=f"{status} evidence",
    )


POLICY = ObjectivePolicy(
    latency_slo_us=100.0,
    latency_relative_floor=0.03,
    throughput_relative_floor=0.01,
)


class ObjectiveTrialTest(unittest.TestCase):
    def test_always_uses_client_latency_and_server_throughput(self) -> None:
        client = types.SimpleNamespace(
            spec=types.SimpleNamespace(endpoint_id="loadgen", role="client"),
            latency={"p999_us": statistic(9.0, unit="us")},
            throughput=statistic(19.0, unit="Mpps"),
        )
        server = types.SimpleNamespace(
            spec=types.SimpleNamespace(endpoint_id="target", role="server"),
            latency={"p999_us": statistic(90.0, unit="us")},
            throughput=statistic(31.0, unit="Mpps"),
        )
        for target, peer in ((client, server), (server, client)):
            with self.subTest(target_role=target.spec.role):
                summary = types.SimpleNamespace(
                    trial_id=f"trial-{target.spec.role}",
                    target=target,
                    peer=peer,
                    peer_health=types.SimpleNamespace(healthy=True, reasons=()),
                )
                evidence = objective_trial_from_summary(summary)
                self.assertEqual(evidence.status, "valid")
                self.assertEqual(evidence.client_p999.median, 9.0)
                self.assertEqual(evidence.server_throughput.median, 31.0)

    def test_marks_unhealthy_peer_as_rejected_evidence(self) -> None:
        endpoint = types.SimpleNamespace(
            spec=types.SimpleNamespace(endpoint_id="target", role="client"),
            latency={"p999_us": statistic(9.0, unit="us")},
            throughput=statistic(31.0, unit="Mpps"),
        )
        summary = types.SimpleNamespace(
            trial_id="trial-unhealthy",
            target=endpoint,
            peer=types.SimpleNamespace(
                spec=types.SimpleNamespace(endpoint_id="peer", role="server"),
                latency={"p999_us": statistic(10.0, unit="us")},
                throughput=statistic(30.0, unit="Mpps"),
            ),
            peer_health=types.SimpleNamespace(
                healthy=False,
                reasons=("throughput: endpoint medians differ",),
            ),
        )
        evidence = objective_trial_from_summary(summary)
        self.assertEqual(evidence.status, "unhealthy_peer")
        self.assertIsNone(evidence.client_p999)
        self.assertIn("throughput", evidence.rejection_reason)


class CandidateComparisonTest(unittest.TestCase):
    def test_infeasible_objective_prefers_latency_then_feasibility(self) -> None:
        accepted = trial("accepted", latency_us=120.0, throughput_mpps=30.0)

        improved = compare_candidate(
            accepted,
            trial("improved", latency_us=110.0, throughput_mpps=20.0),
            POLICY,
        )
        self.assertTrue(improved.accepted)
        self.assertEqual(improved.metric, "client_p999_us")

        noisy = compare_candidate(
            accepted,
            trial("noisy", latency_us=118.0, throughput_mpps=40.0),
            POLICY,
        )
        self.assertFalse(noisy.accepted)
        self.assertEqual(noisy.reason, "latency improvement is not significant")

        feasible = compare_candidate(
            accepted,
            trial("feasible", latency_us=99.0, throughput_mpps=10.0),
            POLICY,
        )
        self.assertTrue(feasible.accepted)
        self.assertEqual(feasible.reason, "candidate reaches latency feasibility")

        noisy_boundary_crossing = compare_candidate(
            trial(
                "near-slo",
                latency_us=100.1,
                throughput_mpps=30.0,
                latency_mad=1.0,
            ),
            trial(
                "barely-feasible",
                latency_us=99.9,
                throughput_mpps=100.0,
                latency_mad=1.0,
            ),
            POLICY,
        )
        self.assertFalse(noisy_boundary_crossing.accepted)
        self.assertTrue(noisy_boundary_crossing.candidate_feasible)
        self.assertEqual(
            noisy_boundary_crossing.reason,
            "latency improvement is not significant",
        )

    def test_feasible_objective_keeps_slo_and_maximizes_server_throughput(self) -> None:
        accepted = trial("accepted", latency_us=10.0, throughput_mpps=20.0)

        faster = compare_candidate(
            accepted,
            trial("faster", latency_us=20.0, throughput_mpps=21.0),
            POLICY,
        )
        self.assertTrue(faster.accepted)
        self.assertEqual(faster.metric, "server_throughput_mpps")

        slo_violation = compare_candidate(
            accepted,
            trial("slow-latency", latency_us=101.0, throughput_mpps=50.0),
            POLICY,
        )
        self.assertFalse(slo_violation.accepted)
        self.assertEqual(slo_violation.reason, "candidate violates latency SLO")

        noisy = compare_candidate(
            accepted,
            trial("noisy", latency_us=9.0, throughput_mpps=20.1),
            POLICY,
        )
        self.assertFalse(noisy.accepted)
        self.assertEqual(noisy.reason, "throughput improvement is not significant")

    def test_three_mad_noise_can_dominate_relative_floor(self) -> None:
        accepted = trial(
            "accepted",
            latency_us=10.0,
            throughput_mpps=20.0,
            throughput_mad=2.0,
        )
        comparison = compare_candidate(
            accepted,
            trial("candidate", latency_us=10.0, throughput_mpps=25.0),
            POLICY,
        )
        self.assertFalse(comparison.accepted)
        self.assertEqual(comparison.required_improvement, 6.0)

    def test_relative_noise_floor_is_symmetric(self) -> None:
        policy = ObjectivePolicy(
            latency_slo_us=100.0,
            latency_relative_floor=0.03,
            throughput_relative_floor=0.20,
        )
        comparison = compare_candidate(
            trial("accepted", latency_us=10.0, throughput_mpps=20.0),
            trial("candidate", latency_us=10.0, throughput_mpps=25.0),
            policy,
        )
        self.assertFalse(comparison.accepted)
        self.assertEqual(comparison.required_improvement, 5.0)

    def test_rejected_trials_never_become_performance_evidence(self) -> None:
        accepted = trial("accepted", latency_us=10.0, throughput_mpps=20.0)
        for status in ("invalid", "unhealthy_peer", "infrastructure_failure"):
            with self.subTest(status=status):
                comparison = compare_candidate(
                    accepted, rejected(f"trial-{status}", status), POLICY
                )
                self.assertFalse(comparison.accepted)
                self.assertEqual(comparison.metric, None)
                self.assertIn(status, comparison.reason)


class HistoricalBestTest(unittest.TestCase):
    def test_prefers_highest_throughput_feasible_trial(self) -> None:
        baseline = trial("infeasible-fast", latency_us=110.0, throughput_mpps=100.0)
        accepted_trials = (
            trial("feasible-a", latency_us=20.0, throughput_mpps=30.0),
            trial("feasible-best", latency_us=30.0, throughput_mpps=31.0),
        )
        self.assertEqual(
            select_historical_best(
                baseline=baseline,
                accepted_trials=accepted_trials,
                policy=POLICY,
            ).trial_id,
            "feasible-best",
        )

    def test_uses_lowest_latency_until_any_trial_is_feasible(self) -> None:
        baseline = trial("first", latency_us=120.0, throughput_mpps=40.0)
        accepted_trials = (
            trial("best", latency_us=110.0, throughput_mpps=10.0),
            trial("same-latency-later", latency_us=110.0, throughput_mpps=99.0),
        )
        self.assertEqual(
            select_historical_best(
                baseline=baseline,
                accepted_trials=accepted_trials,
                policy=POLICY,
            ).trial_id,
            "best",
        )

    def test_cannot_promote_a_valid_but_rejected_candidate(self) -> None:
        baseline = trial("baseline", latency_us=10.0, throughput_mpps=20.0)
        accepted = trial("accepted", latency_us=10.0, throughput_mpps=21.0)
        rejected_candidate = trial(
            "noisy-spike",
            latency_us=10.0,
            throughput_mpps=99.0,
            throughput_mad=30.0,
        )
        self.assertFalse(
            compare_candidate(accepted, rejected_candidate, POLICY).accepted
        )
        best = select_historical_best(
            baseline=baseline,
            accepted_trials=(accepted,),
            policy=POLICY,
        )
        self.assertEqual(best.trial_id, "accepted")

    def test_equal_throughput_tie_keeps_first_seen(self) -> None:
        baseline = trial("baseline", latency_us=10.0, throughput_mpps=20.0)
        first = trial("first", latency_us=20.0, throughput_mpps=30.0)
        later = trial("later", latency_us=5.0, throughput_mpps=30.0)
        best = select_historical_best(
            baseline=baseline,
            accepted_trials=(first, later),
            policy=POLICY,
        )
        self.assertEqual(best.trial_id, "first")


if __name__ == "__main__":
    unittest.main()
