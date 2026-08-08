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


if __name__ == "__main__":
    unittest.main()
