from __future__ import annotations

import unittest

from artifact_eval.matrices import end_to_end_cases, figure3_cases, figure6_cases
from artifact_eval.model import profile_defaults


class EndToEndMatrixTest(unittest.TestCase):
    def test_matrix_contains_six_handlers_on_both_backends(self) -> None:
        profile = profile_defaults("smoke", experiment="e2e")
        cases = end_to_end_cases(profile, sessions=1)

        self.assertEqual(len(cases), 12)
        self.assertEqual(
            {(case.configuration.backend, case.configuration.handler) for case in cases},
            {
                (backend, handler)
                for backend in ("dpdk", "roce")
                for handler in (
                    "t_app",
                    "l_app",
                    "m_app",
                    "file_write",
                    "file_read",
                    "key_value",
                )
            },
        )
        self.assertTrue(
            all(
                (case.configuration.c1, case.configuration.c2) == (16, 16)
                and case.mode == "bootstrap"
                and case.tuning_rounds == 2
                for case in cases
            )
        )

    def test_figure3_matrix_has_three_independent_axes(self) -> None:
        profile = profile_defaults("paper", experiment="figure3")
        cases = figure3_cases(profile)

        self.assertEqual(len(cases), 12)
        self.assertEqual(
            [(case.configuration.c1, case.configuration.c2, case.configuration.c3) for case in cases],
            [
                (4, 4, 32), (8, 4, 32), (12, 4, 32), (16, 4, 32),
                (16, 4, 32), (16, 8, 32), (16, 12, 32), (16, 16, 32),
                (8, 4, 16), (8, 4, 32), (8, 4, 64), (8, 4, 128),
            ],
        )
        self.assertTrue(all(case.repeats == 20 for case in cases))

    def test_figure6_uses_the_endpoint_that_owns_each_measured_stage(self) -> None:
        profile = profile_defaults("smoke", experiment="figure6")
        cases = figure6_cases(profile)

        self.assertEqual(len(cases), 10)
        l_app = [case for case in cases if case.configuration.handler == "l_app"]
        m_app = [case for case in cases if case.configuration.handler == "m_app"]
        self.assertTrue(all(case.target_role == "client" for case in l_app))
        self.assertTrue(all(case.target_role == "server" for case in m_app))
        self.assertEqual(
            [case.configuration.c1 for case in l_app], [1, 2, 4, 8, 16]
        )


if __name__ == "__main__":
    unittest.main()
