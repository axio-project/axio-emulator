from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class DocumentationContractTest(unittest.TestCase):
    def test_release_version_is_1_3_1(self) -> None:
        self.assertEqual((ROOT / "VERSION").read_text().strip(), "1.3.1")

    def test_topology_search_policy_is_user_visible(self) -> None:
        readme = (ROOT / "README.md").read_text()
        guide = (ROOT / "docs/pipetune.md").read_text()
        documentation = " ".join(f"{readme}\n{guide}".split())

        for statement in (
            "NUMA workspace budget `U`",
            "equivalent throughput while releasing physical cores",
            "one-to-one split",
            "complete balanced application fanout layer",
            "dispatcher expansion remains one-to-one",
            "Failed trials never replace `best.toml`",
        ):
            with self.subTest(statement=statement):
                self.assertIn(statement, documentation)

        self.assertIn(
            "If the accepted baseline violates the latency SLO, any "
            "candidate must significantly reduce client P99.9",
            documentation,
        )
        self.assertIn(
            "The equivalent-throughput/fewer-core exception applies only "
            "to count reduction",
            documentation,
        )
        self.assertIn(
            "except that a count reduction may preserve equivalent "
            "throughput when it releases physical cores",
            documentation,
        )
        self.assertIn(
            "C3 does not release a core and therefore still needs a "
            "significant throughput gain once the accepted baseline is "
            "latency feasible; before feasibility, it must instead "
            "significantly reduce client P99.9",
            documentation,
        )
        self.assertIn(
            "memory_candidates_exhausted_without_compute_evidence",
            documentation,
        )


if __name__ == "__main__":
    unittest.main()
