from __future__ import annotations

import unittest

from artifact_eval.matrices import end_to_end_cases
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


if __name__ == "__main__":
    unittest.main()
