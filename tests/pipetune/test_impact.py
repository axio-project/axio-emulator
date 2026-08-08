from __future__ import annotations

import types
import unittest

from pipetune.diagnosis import Diagnosis, StageComponent, Statistic
from pipetune.impact import compare_expected_impact


def _statistic(value: float, uncertainty: float, unit: str) -> Statistic:
    return Statistic(
        samples=(value,),
        median=value,
        mad=0.0,
        uncertainty=uncertainty,
        unit=unit,
    )


def _diagnosis(point: str, direction: str) -> Diagnosis:
    return Diagnosis(
        schema="pipetune.diagnosis/v1",
        point=point,
        direction=direction,
        confidence="high",
        evidence=(),
        rejected_evidence=(),
        missing_metrics=(),
        noise_thresholds={},
        required_probe=None,
        input_hashes={},
        stage_ranking=(),
    )


class _Endpoint:
    def __init__(self, component: StageComponent):
        self.dominant_component = component
        self._components = {component.name: component}

    def component(self, name: str) -> StageComponent:
        return self._components[name]


def _summary(
    *,
    component_name: str = "app_rx.stall",
    component_kind: str = "stall",
    component_value: float = 0.10,
    component_uncertainty: float = 0.01,
    counters: dict[str, Statistic] | None = None,
):
    component = StageComponent(
        name=component_name,
        stage=component_name.split(".", 1)[0],
        kind=component_kind,
        direction="rx" if "rx" in component_name else "tx",
        statistic=_statistic(
            component_value,
            component_uncertainty,
            "us/packet",
        ),
    )
    return types.SimpleNamespace(
        target=_Endpoint(component),
        counters={} if counters is None else counters,
    )


class ExpectedImpactTest(unittest.TestCase):
    def test_p1_requires_the_dominant_stall_to_significantly_decrease(self) -> None:
        accepted = compare_expected_impact(
            _diagnosis("P1", "rx"),
            _summary(component_value=0.10),
            _summary(component_value=0.06),
            candidate_id="candidate-good",
        )
        rejected = compare_expected_impact(
            _diagnosis("P1", "rx"),
            _summary(component_value=0.10),
            _summary(component_value=0.095),
            candidate_id="candidate-noisy",
        )

        self.assertTrue(accepted.accepted)
        self.assertEqual(accepted.metric, "app_rx.stall")
        self.assertAlmostEqual(accepted.observed_reduction, 0.04)
        self.assertFalse(rejected.accepted)
        self.assertIn("not significant", rejected.reason)

    def test_p2_and_p4_require_directional_llc_miss_rate_to_decrease(self) -> None:
        for point, direction, counter in (
            ("P2", "tx", "llc_store"),
            ("P2", "rx", "llc_load"),
            ("P4", "tx", "llc_store"),
            ("P4", "rx", "llc_load"),
        ):
            with self.subTest(point=point, direction=direction):
                comparison = compare_expected_impact(
                    _diagnosis(point, direction),
                    _summary(
                        counters={
                            counter: _statistic(60.0, 0.5, "percentage points")
                        }
                    ),
                    _summary(
                        counters={
                            counter: _statistic(55.0, 0.5, "percentage points")
                        }
                    ),
                    candidate_id=f"candidate-{point}-{direction}",
                )
                self.assertTrue(comparison.accepted)
                self.assertEqual(comparison.metric, counter)

    def test_p3_requires_directional_io_miss_rate_to_decrease(self) -> None:
        for direction, counter in (("tx", "io_read"), ("rx", "io_write")):
            with self.subTest(direction=direction):
                comparison = compare_expected_impact(
                    _diagnosis("P3", direction),
                    _summary(
                        counters={
                            counter: _statistic(40.0, 1.0, "percentage points")
                        }
                    ),
                    _summary(
                        counters={
                            counter: _statistic(30.0, 1.0, "percentage points")
                        }
                    ),
                    candidate_id=f"candidate-{direction}",
                )
                self.assertTrue(comparison.accepted)
                self.assertEqual(comparison.metric, counter)

    def test_missing_or_increasing_expected_metric_rejects_the_candidate(self) -> None:
        missing = compare_expected_impact(
            _diagnosis("P3", "rx"),
            _summary(
                counters={
                    "io_write": _statistic(40.0, 1.0, "percentage points")
                }
            ),
            _summary(),
            candidate_id="candidate-missing",
        )
        increasing = compare_expected_impact(
            _diagnosis("P2", "rx"),
            _summary(
                counters={
                    "llc_load": _statistic(40.0, 1.0, "percentage points")
                }
            ),
            _summary(
                counters={
                    "llc_load": _statistic(45.0, 1.0, "percentage points")
                }
            ),
            candidate_id="candidate-increasing",
        )

        self.assertFalse(missing.accepted)
        self.assertIn("unavailable", missing.reason)
        self.assertFalse(increasing.accepted)
        self.assertLess(increasing.observed_reduction, 0.0)


if __name__ == "__main__":
    unittest.main()
