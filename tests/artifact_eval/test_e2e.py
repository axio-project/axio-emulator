from __future__ import annotations

import unittest

from artifact_eval.matrices import (
    end_to_end_cases,
    figure3_cases,
    figure6_cases,
    figure7_cases,
    figure8_cases,
    figure14_cases,
)
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
        file_read = [
            case for case in cases if case.configuration.handler == "file_read"
        ]
        self.assertTrue(all(case.configuration.c3 == 1 for case in file_read))

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

    def test_figure7_scales_colocated_app_rx_topologies(self) -> None:
        cases = figure7_cases(profile_defaults("smoke", experiment="figure7"))

        self.assertEqual(len(cases), 10)
        self.assertTrue(all(case.target_role == "server" for case in cases))
        self.assertEqual(
            [
                (case.configuration.handler, case.configuration.c1, case.configuration.c2, case.configuration.c3)
                for case in cases
            ],
            [
                (handler, value, value, 128)
                for handler in ("l_app", "t_app")
                for value in (1, 2, 4, 8, 16)
            ],
        )

    def test_figure8_sweeps_c3_for_two_stage_owners(self) -> None:
        cases = figure8_cases(profile_defaults("smoke", experiment="figure8"))

        self.assertEqual(len(cases), 10)
        t_app = cases[:5]
        l_app = cases[5:]
        self.assertTrue(all(case.target_role == "server" for case in t_app))
        self.assertTrue(all(case.target_role == "client" for case in l_app))
        self.assertEqual(
            [case.configuration.c3 for case in t_app], [32, 64, 128, 256, 512]
        )
        self.assertEqual(
            [case.configuration.c3 for case in l_app], [16, 32, 64, 128, 256]
        )

    def test_figure14_contains_two_bounded_adaptation_trajectories(self) -> None:
        cases = figure14_cases(profile_defaults("paper", experiment="figure14"))

        self.assertEqual(len(cases), 2)
        self.assertEqual(
            [
                (
                    case.configuration.case_id,
                    case.configuration.backend,
                    case.configuration.handler,
                    case.configuration.packet_handler,
                    case.tuning_rounds,
                )
                for case in cases
            ],
            [
                ("dpdk-packet-echo", "dpdk", "t_app", "echo", 5),
                ("roce-file-write", "roce", "file_write", "empty", 5),
            ],
        )


if __name__ == "__main__":
    unittest.main()
