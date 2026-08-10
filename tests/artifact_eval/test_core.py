from __future__ import annotations

import pathlib
import tempfile
import unittest

from artifact_eval.configuration import CaseConfiguration, common_overrides, target_overrides
from artifact_eval.manifest import ManifestError, RunManifest
from artifact_eval.model import profile_defaults


class ArtifactEvaluationCoreTest(unittest.TestCase):
    def test_profiles_publish_reproducible_defaults(self) -> None:
        smoke = profile_defaults("smoke", experiment="figure3")
        paper = profile_defaults("paper", experiment="figure3")
        e2e = profile_defaults("paper", experiment="e2e")

        self.assertEqual((smoke.warmup_windows, smoke.sample_windows), (2, 3))
        self.assertEqual(smoke.repeats, 1)
        self.assertEqual(paper.repeats, 20)
        self.assertEqual(e2e.tuning_rounds, 20)

    def test_case_overrides_separate_common_build_values_from_target_topology(self) -> None:
        case = CaseConfiguration(
            case_id="m-app-c1-8",
            backend="roce",
            handler="m_app",
            c1=8,
            c2=1,
            c3=16,
            warmup_windows=2,
            sample_windows=3,
            stage_distribution=True,
        )

        common = common_overrides(case)
        target = target_overrides(case)
        self.assertEqual(common["network.backend"], "roce")
        self.assertEqual(common["knobs.build.mempool_handler"], "huge_alloc")
        self.assertEqual(common["other.mempool_cache_size"], 0)
        self.assertTrue(common["metrics.stage_distribution.enabled"])
        self.assertNotIn("knobs.runtime.application_core_count", common)
        self.assertEqual(target["knobs.runtime.application_core_count"], 8)
        self.assertEqual(target["knobs.runtime.dispatcher_queue_count"], 1)
        self.assertEqual(target["knobs.runtime.nic_rx_post_size"], 16)

    def test_resume_requires_identical_identity_and_complete_case_artifacts(self) -> None:
        with tempfile.TemporaryDirectory(prefix="ae-manifest-") as temp_dir:
            root = pathlib.Path(temp_dir) / "run"
            manifest = RunManifest.create(
                root,
                experiment="figure3",
                profile="smoke",
                git_commit="a" * 40,
                matrix_fingerprint="b" * 64,
                cases=("c1-4", "c1-8"),
            )
            case_dir = root / "cases/c1-4"
            case_dir.mkdir(parents=True)
            artifact = case_dir / "session.json"
            artifact.write_text("{}\n", encoding="utf-8")
            manifest.complete_case("c1-4", (artifact,))

            resumed = RunManifest.resume(
                root,
                experiment="figure3",
                profile="smoke",
                git_commit="a" * 40,
                matrix_fingerprint="b" * 64,
            )
            self.assertEqual(resumed.completed_cases(), ("c1-4",))

            artifact.write_text("changed\n", encoding="utf-8")
            with self.assertRaises(ManifestError):
                RunManifest.resume(
                    root,
                    experiment="figure3",
                    profile="smoke",
                    git_commit="a" * 40,
                    matrix_fingerprint="b" * 64,
                )


if __name__ == "__main__":
    unittest.main()
