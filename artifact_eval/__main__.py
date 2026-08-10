"""Command-line interface for Axio artifact evaluation."""

from __future__ import annotations

import argparse
import pathlib
import sys
from collections.abc import Sequence

from artifact_eval.harness import ArtifactHarness, HarnessError, HarnessOptions, ensure_configure_binary
from artifact_eval.matrices import end_to_end_cases
from artifact_eval.model import ModelError, profile_defaults
from artifact_eval.summary import SummaryError, write_e2e_summary


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="artifact-eval")
    parser.add_argument("experiment", choices=("e2e",))
    parser.add_argument("--profile", choices=("smoke", "paper"), default="smoke")
    destination = parser.add_mutually_exclusive_group(required=True)
    destination.add_argument("--output")
    destination.add_argument("--resume")
    parser.add_argument("--config-dir")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--warmup-windows", type=int)
    parser.add_argument("--sample-windows", type=int)
    parser.add_argument("--repeats", type=int)
    parser.add_argument("--tuning-rounds", type=int)
    parser.add_argument("--sessions", type=int, default=1)
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
        profile = profile_defaults(arguments.profile, experiment=arguments.experiment)
        if arguments.experiment == "e2e":
            cases = end_to_end_cases(
                profile,
                sessions=arguments.sessions,
                warmup_windows=arguments.warmup_windows,
                sample_windows=arguments.sample_windows,
                tuning_rounds=arguments.tuning_rounds,
            )
        else:
            raise AssertionError(arguments.experiment)
        output = pathlib.Path(arguments.resume or arguments.output)
        config_dir = pathlib.Path(arguments.config_dir) if arguments.config_dir else (
            repository / "config/artifact/reference-200g"
        )
        options = HarnessOptions(
            repository=repository,
            config_dir=config_dir,
            output=output,
            profile=arguments.profile,
            resume=arguments.resume is not None,
            dry_run=arguments.dry_run,
        )
        manifest = ArtifactHarness(options).execute(arguments.experiment, cases)
        if manifest is not None:
            write_e2e_summary(
                manifest.root,
                cases,
                configure_binary=ensure_configure_binary(repository),
            )
    except (HarnessError, ModelError, SummaryError, OSError, ValueError) as error:
        print(f"artifact-eval: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
