from __future__ import annotations

import unittest

from pipetune.paired_search import (
    PairedSearchError,
    PairedSearchState,
    PressureClass,
    PressureSample,
    SearchMode,
    classify_pressure,
)


def _sample(
    count: int,
    llc: float | None,
    io: float | None,
    *,
    uncertainty: float = 0.5,
) -> PressureSample:
    return PressureSample(
        count=count,
        llc_rate=llc,
        io_rate=io,
        llc_uncertainty=uncertainty if llc is not None else None,
        io_uncertainty=uncertainty if io is not None else None,
    )


class PressureClassificationTest(unittest.TestCase):
    def test_requires_both_directional_rates_to_cross_the_threshold(self) -> None:
        self.assertEqual(
            classify_pressure(_sample(8, 39.0, 40.0)),
            PressureClass.RELIEVED,
        )
        self.assertEqual(
            classify_pressure(_sample(8, 41.0, 42.0)),
            PressureClass.STRONG_BOUND,
        )
        self.assertEqual(
            classify_pressure(_sample(8, 39.0, 41.0)),
            PressureClass.MIXED,
        )

    def test_missing_rate_is_unavailable_not_relieved(self) -> None:
        for sample in (_sample(8, None, 30.0), _sample(8, 30.0, None)):
            with self.subTest(sample=sample):
                self.assertEqual(
                    classify_pressure(sample),
                    PressureClass.UNAVAILABLE,
                )


class PairedSearchStateTest(unittest.TestCase):
    def test_binary_search_crosses_e2e_valley_and_selects_largest_relief(self) -> None:
        state = PairedSearchState.start(direction="rx", count=16)
        self.assertEqual(state.next_count, 15)

        state = state.observe(_sample(15, 80.0, 90.0), objective_improved=False)
        self.assertEqual((state.mode, state.next_count), (SearchMode.BINARY_SEEK, 8))

        state = state.observe(_sample(8, 35.0, 38.0), objective_improved=False)
        self.assertEqual(
            (state.mode, state.low_relief_count, state.high_pressure_count, state.next_count),
            (SearchMode.BINARY_REFINE, 8, 15, 11),
        )
        state = state.observe(_sample(11, 45.0, 55.0), objective_improved=False)
        self.assertEqual((state.high_pressure_count, state.next_count), (11, 9))
        state = state.observe(_sample(9, 39.0, 42.0), objective_improved=False)

        self.assertEqual(state.mode, SearchMode.COMPUTE)
        self.assertEqual(state.selected_count, 8)
        self.assertIsNone(state.next_count)

    def test_significant_e2e_gain_keeps_one_core_descent(self) -> None:
        state = PairedSearchState.start(direction="tx", count=16)
        state = state.observe(_sample(15, 70.0, 80.0), objective_improved=True)

        self.assertEqual(state.mode, SearchMode.LINEAR)
        self.assertEqual(state.next_count, 14)

    def test_first_large_step_must_reduce_at_least_one_rate(self) -> None:
        state = PairedSearchState.start(direction="rx", count=16)
        state = state.observe(_sample(15, 80.0, 90.0), objective_improved=False)
        state = state.observe(_sample(8, 79.8, 90.2), objective_improved=False)

        self.assertEqual(state.mode, SearchMode.FAILED)
        self.assertEqual(state.failure_reason, "contention_not_relieved")

    def test_threshold_relief_is_not_rejected_by_measurement_uncertainty(self) -> None:
        state = PairedSearchState.start(direction="rx", count=16)
        state = state.observe(
            _sample(15, 41.0, 41.0, uncertainty=3.0),
            objective_improved=False,
        )

        state = state.observe(
            _sample(8, 39.0, 39.0, uncertainty=3.0),
            objective_improved=False,
        )

        self.assertEqual(state.mode, SearchMode.BINARY_REFINE)
        self.assertEqual(state.low_relief_count, 8)
        self.assertEqual(state.next_count, 11)

    def test_rejects_inconsistent_persisted_binary_state(self) -> None:
        state = PairedSearchState.start(direction="rx", count=16)
        state = state.observe(_sample(15, 80.0, 90.0), objective_improved=False)
        document = state.to_document()
        document["low_relief_count"] = 9
        document["high_pressure_count"] = 2

        with self.assertRaisesRegex(PairedSearchError, "binary bounds"):
            PairedSearchState.from_document(document)

    def test_count_one_without_relief_is_threshold_unreachable(self) -> None:
        state = PairedSearchState.start(direction="rx", count=2)
        state = state.observe(_sample(1, 60.0, 70.0), objective_improved=True)

        self.assertEqual(state.mode, SearchMode.FAILED)
        self.assertEqual(state.failure_reason, "threshold_unreachable")

    def test_state_round_trips_for_resume(self) -> None:
        state = PairedSearchState.start(direction="rx", count=16)
        state = state.observe(_sample(15, 80.0, 90.0), objective_improved=False)

        self.assertEqual(PairedSearchState.from_document(state.to_document()), state)


if __name__ == "__main__":
    unittest.main()
