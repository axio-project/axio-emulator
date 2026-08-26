from __future__ import annotations

import types
import unittest

from pipetune.diagnosis import Diagnosis, StageComponent, Statistic
from pipetune.impact import compare_expected_impact
from pipetune.search_policy import ImpactSpec


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
    def __init__(self, components: tuple[StageComponent, ...]):
        self.leading_component = components[0]
        self._components = {component.name: component for component in components}

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
        target=_Endpoint((component,)),
        counters={} if counters is None else counters,
    )


def _pipeline_summary(
    values: dict[str, float],
    *,
    uncertainty: float = 0.005,
):
    components = tuple(
        StageComponent(
            name=name,
            stage=name.split(".", 1)[0],
            kind="stall",
            direction="rx" if "_rx." in name else "tx",
            statistic=_statistic(value, uncertainty, "us/packet"),
        )
        for name, value in values.items()
    )
    return types.SimpleNamespace(
        target=_Endpoint(components),
        counters={},
        noise_thresholds={"stall_time_relative_floor": 0.05},
    )


def _pipeline_sample_summary(
    samples: dict[str, tuple[tuple[float, ...], float]],
):
    components = tuple(
        StageComponent(
            name=name,
            stage=name.split(".", 1)[0],
            kind="stall",
            direction="rx" if "_rx." in name else "tx",
            statistic=Statistic(
                samples=values,
                median=median,
                mad=0.0,
                uncertainty=0.0,
                unit="us/packet",
            ),
        )
        for name, (values, median) in samples.items()
    )
    return types.SimpleNamespace(
        target=_Endpoint(components),
        counters={},
        noise_thresholds={"stall_time_relative_floor": 0.05},
    )


class ExpectedImpactTest(unittest.TestCase):
    def test_counter_spec_requires_the_named_counter_to_decrease(self) -> None:
        comparison = compare_expected_impact(
            ImpactSpec("counter", "llc_load", "rx"),
            _summary(
                counters={
                    "llc_load": _statistic(70.0, 0.5, "percentage points")
                }
            ),
            _summary(
                counters={
                    "llc_load": _statistic(60.0, 0.5, "percentage points")
                }
            ),
            candidate_id="paired-rx",
        )

        self.assertTrue(comparison.accepted)
        self.assertEqual(comparison.metric, "llc_load")

    def test_pipeline_stall_sums_the_same_four_components(self) -> None:
        baseline = _pipeline_summary(
            {
                "app_rx.stall": 0.03,
                "app_tx.stall": 0.02,
                "dispatcher_rx.stall": 0.04,
                "dispatcher_tx.stall": 0.03,
            }
        )
        candidate = _pipeline_summary(
            {
                "app_rx.stall": 0.02,
                "app_tx.stall": 0.02,
                "dispatcher_rx.stall": 0.02,
                "dispatcher_tx.stall": 0.02,
            }
        )

        comparison = compare_expected_impact(
            ImpactSpec("pipeline_stall", "pipeline_stall", "rx"),
            baseline,
            candidate,
            candidate_id="split",
        )

        self.assertTrue(comparison.accepted)
        self.assertEqual(comparison.metric, "pipeline_stall")
        self.assertAlmostEqual(comparison.baseline_value, 0.12)
        self.assertAlmostEqual(comparison.candidate_value, 0.08)
        self.assertAlmostEqual(comparison.observed_reduction, 0.04)

    def test_pipeline_stall_aggregates_each_window_before_statistics(self) -> None:
        baseline = _pipeline_sample_summary(
            {
                "app_rx.stall": ((0.0, 0.0, 0.06), 0.0),
                "app_tx.stall": ((0.0, 0.06, 0.0), 0.0),
                "dispatcher_rx.stall": ((0.06, 0.0, 0.0), 0.0),
                "dispatcher_tx.stall": ((0.0, 0.0, 0.0), 0.0),
            }
        )
        candidate = _pipeline_sample_summary(
            {
                name: ((0.01, 0.01, 0.01), 0.01)
                for name in (
                    "app_rx.stall",
                    "app_tx.stall",
                    "dispatcher_rx.stall",
                    "dispatcher_tx.stall",
                )
            }
        )

        comparison = compare_expected_impact(
            ImpactSpec("pipeline_stall", "pipeline_stall", "rx"),
            baseline,
            candidate,
            candidate_id="window-aligned-split",
        )

        self.assertTrue(comparison.accepted)
        self.assertAlmostEqual(comparison.baseline_value, 0.06)
        self.assertAlmostEqual(comparison.candidate_value, 0.04)
        self.assertAlmostEqual(comparison.required_reduction, 0.003)

    def test_pipeline_stall_rejects_no_reduction_or_missing_component(self) -> None:
        values = {
            "app_rx.stall": 0.03,
            "app_tx.stall": 0.02,
            "dispatcher_rx.stall": 0.04,
            "dispatcher_tx.stall": 0.03,
        }
        no_reduction = compare_expected_impact(
            ImpactSpec("pipeline_stall", "pipeline_stall", "rx"),
            _pipeline_summary(values),
            _pipeline_summary(values),
            candidate_id="unchanged",
        )
        missing_values = dict(values)
        missing_values.pop("dispatcher_tx.stall")
        missing = compare_expected_impact(
            ImpactSpec("pipeline_stall", "pipeline_stall", "rx"),
            _pipeline_summary(values),
            _pipeline_summary(missing_values),
            candidate_id="missing",
        )

        self.assertFalse(no_reduction.accepted)
        self.assertIn("not significant", no_reduction.reason)
        self.assertFalse(missing.accepted)
        self.assertIn("unavailable", missing.reason)

        incomplete_baseline = compare_expected_impact(
            ImpactSpec("pipeline_stall", "pipeline_stall", "rx"),
            _pipeline_summary(missing_values),
            _pipeline_summary(
                {name: value / 2.0 for name, value in missing_values.items()}
            ),
            candidate_id="incomplete-baseline",
        )

        self.assertFalse(incomplete_baseline.accepted)
        self.assertIn("unavailable", incomplete_baseline.reason)

    def test_pipeline_stall_rejects_misaligned_window_series(self) -> None:
        names = (
            "app_rx.stall",
            "app_tx.stall",
            "dispatcher_rx.stall",
            "dispatcher_tx.stall",
        )
        baseline = _pipeline_sample_summary(
            {name: ((0.03, 0.03, 0.03), 0.03) for name in names}
        )
        candidate = _pipeline_sample_summary(
            {
                name: (
                    (
                        (0.01, 0.01)
                        if name == "app_rx.stall"
                        else (0.01, 0.01, 0.01)
                    ),
                    0.01,
                )
                for name in names
            }
        )

        comparison = compare_expected_impact(
            ImpactSpec("pipeline_stall", "pipeline_stall", "rx"),
            baseline,
            candidate,
            candidate_id="misaligned-split",
        )

        self.assertFalse(comparison.accepted)
        self.assertIn("unavailable", comparison.reason)

    def test_component_spec_requires_named_completion_to_decrease(self) -> None:
        for metric in ("app_rx.completion", "dispatcher_tx.completion"):
            with self.subTest(metric=metric):
                comparison = compare_expected_impact(
                    ImpactSpec(
                        kind="component",
                        metric=metric,
                        direction="rx" if "_rx." in metric else "tx",
                    ),
                    _summary(
                        component_name=metric,
                        component_kind="completion",
                        component_value=0.10,
                    ),
                    _summary(
                        component_name=metric,
                        component_kind="completion",
                        component_value=0.06,
                    ),
                    candidate_id=f"candidate-{metric}",
                )
                self.assertTrue(comparison.accepted)
                self.assertEqual(comparison.point, "component")
                self.assertEqual(comparison.metric, metric)
                self.assertAlmostEqual(comparison.observed_reduction, 0.04)

    def test_component_spec_rejects_missing_or_wrong_kind_metric(self) -> None:
        spec = ImpactSpec("component", "app_rx.completion", "rx")
        missing = compare_expected_impact(
            spec,
            _summary(
                component_name="app_rx.completion",
                component_kind="completion",
            ),
            _summary(
                component_name="app_tx.completion",
                component_kind="completion",
            ),
            candidate_id="candidate-missing",
        )
        wrong_kind = compare_expected_impact(
            spec,
            _summary(
                component_name="app_rx.completion",
                component_kind="stall",
            ),
            _summary(
                component_name="app_rx.completion",
                component_kind="stall",
                component_value=0.01,
            ),
            candidate_id="candidate-wrong-kind",
        )

        self.assertFalse(missing.accepted)
        self.assertIn("unavailable", missing.reason)
        self.assertFalse(wrong_kind.accepted)
        self.assertIn("unavailable", wrong_kind.reason)

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
