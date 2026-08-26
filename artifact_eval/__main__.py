"""Command-line interface for Axio artifact evaluation."""

from __future__ import annotations

import argparse
import pathlib
import sys
from collections.abc import Sequence

from artifact_eval.harness import (
    ArtifactHarness,
    ExperimentCase,
    HarnessError,
    HarnessOptions,
    ensure_configure_binary,
    report_progress,
)
from artifact_eval.manifest import RunManifest
from artifact_eval.matrices import (
    end_to_end_cases,
    figure3_cases,
    figure6_cases,
    figure7_cases,
    figure8_cases,
    figure14_cases,
)
from artifact_eval.model import profile_defaults
from artifact_eval.summary import (
    write_e2e_summary,
    write_figure3_summary,
    write_stage_figure_summary,
)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="artifact-eval")
    parser.add_argument(
        "experiment",
        choices=("e2e", "figure3", "figure6", "figure7", "figure8", "figure14"),
    )
    parser.add_argument("--profile", choices=("smoke", "paper"))
    destination = parser.add_mutually_exclusive_group(required=True)
    destination.add_argument("--output")
    destination.add_argument("--resume")
    parser.add_argument("--config-dir")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--warmup-windows", type=int)
    parser.add_argument("--sample-windows", type=int)
    parser.add_argument("--repeats", type=int)
    parser.add_argument("--tuning-rounds", type=int)
    parser.add_argument("--sessions", type=int)
    parser.add_argument(
        "--case",
        dest="case_id",
        help="run one case ID from the selected experiment matrix",
    )
    return parser


def _positive(value: int | None, name: str, *, zero_allowed: bool = False) -> None:
    if value is None:
        return
    minimum = 0 if zero_allowed else 1
    if value < minimum:
        raise HarnessError(f"{name} must be at least {minimum}")


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    repository = pathlib.Path(__file__).resolve().parents[1]
    try:
        _positive(arguments.warmup_windows, "warmup windows", zero_allowed=True)
        _positive(arguments.sample_windows, "sample windows")
        _positive(arguments.repeats, "repeats")
        _positive(arguments.tuning_rounds, "tuning rounds")
        _positive(arguments.sessions, "sessions")
        if arguments.resume is not None:
            identity = RunManifest.inspect(pathlib.Path(arguments.resume))
            if identity.experiment != arguments.experiment:
                raise HarnessError(
                    f"resume contains {identity.experiment}, not {arguments.experiment}"
                )
            if arguments.profile is not None and arguments.profile != identity.profile:
                raise HarnessError(
                    f"resume profile is {identity.profile}, not {arguments.profile}"
                )
            overrides = {
                "--config-dir": arguments.config_dir,
                "--warmup-windows": arguments.warmup_windows,
                "--sample-windows": arguments.sample_windows,
                "--repeats": arguments.repeats,
                "--tuning-rounds": arguments.tuning_rounds,
                "--sessions": arguments.sessions,
                "--case": arguments.case_id,
            }
            supplied = [name for name, value in overrides.items() if value is not None]
            if supplied:
                raise HarnessError(
                    "resume reconstructs its matrix and inputs; remove "
                    + ", ".join(supplied)
                )
            profile_name = identity.profile
            cases = tuple(
                ExperimentCase.from_document(document)
                for document in identity.matrix
            )
            config_dir = pathlib.Path(arguments.resume) / "inputs/reference-configs"
        else:
            profile_name = arguments.profile or "smoke"
            profile = profile_defaults(profile_name, experiment=arguments.experiment)
            if arguments.experiment == "e2e":
                cases = end_to_end_cases(
                    profile,
                    sessions=arguments.sessions or 1,
                    warmup_windows=arguments.warmup_windows,
                    sample_windows=arguments.sample_windows,
                    tuning_rounds=arguments.tuning_rounds,
                )
            elif arguments.experiment == "figure3":
                cases = figure3_cases(
                    profile,
                    repeats=arguments.repeats,
                    warmup_windows=arguments.warmup_windows,
                    sample_windows=arguments.sample_windows,
                )
            elif arguments.experiment == "figure6":
                cases = figure6_cases(
                    profile,
                    repeats=arguments.repeats,
                    warmup_windows=arguments.warmup_windows,
                    sample_windows=arguments.sample_windows,
                )
            elif arguments.experiment == "figure7":
                cases = figure7_cases(
                    profile,
                    repeats=arguments.repeats,
                    warmup_windows=arguments.warmup_windows,
                    sample_windows=arguments.sample_windows,
                )
            elif arguments.experiment == "figure8":
                cases = figure8_cases(
                    profile,
                    repeats=arguments.repeats,
                    warmup_windows=arguments.warmup_windows,
                    sample_windows=arguments.sample_windows,
                )
            elif arguments.experiment == "figure14":
                cases = figure14_cases(
                    profile,
                    warmup_windows=arguments.warmup_windows,
                    sample_windows=arguments.sample_windows,
                    tuning_rounds=arguments.tuning_rounds,
                )
            else:
                raise AssertionError(arguments.experiment)
            if arguments.case_id is not None:
                selected = tuple(
                    case
                    for case in cases
                    if case.configuration.case_id == arguments.case_id
                )
                if not selected:
                    raise HarnessError(
                        f"unknown {arguments.experiment} case {arguments.case_id!r}"
                    )
                cases = selected
            config_dir = pathlib.Path(arguments.config_dir) if arguments.config_dir else (
                repository / "config/artifact/reference-200g"
            )
        output = pathlib.Path(arguments.resume or arguments.output)
        options = HarnessOptions(
            repository=repository,
            config_dir=config_dir,
            output=output,
            profile=profile_name,
            resume=arguments.resume is not None,
            dry_run=arguments.dry_run,
        )
        manifest = ArtifactHarness(options).execute(arguments.experiment, cases)
        if manifest is not None:
            report_progress(f"write {arguments.experiment} summary")
            if arguments.experiment == "e2e":
                write_e2e_summary(
                    manifest.root,
                    cases,
                    configure_binary=ensure_configure_binary(repository),
                )
            elif arguments.experiment == "figure3":
                write_figure3_summary(manifest.root, cases)
            elif arguments.experiment in ("figure6", "figure7", "figure8"):
                write_stage_figure_summary(
                    manifest.root, cases, figure=arguments.experiment
                )
            elif arguments.experiment == "figure14":
                write_e2e_summary(
                    manifest.root,
                    cases,
                    configure_binary=ensure_configure_binary(repository),
                    title="Figure 14 Axio adaptation",
                    introduction=(
                        "This reports script-based PipeTune bootstrap trajectories for "
                        "Axio packet-echo and file-write datapaths. It does not reproduce "
                        "the unpublished OvS/LineFS probe-event comparison."
                    ),
                )
            manifest.publish_files(
                (
                    manifest.root / "summary.md",
                    manifest.root / "summary.csv",
                    manifest.root / "builds.json",
                )
            )
    except (RuntimeError, OSError, ValueError) as error:
        print(f"artifact-eval: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
