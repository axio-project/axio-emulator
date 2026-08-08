"""Command-line entry point for the script-based PipeTune controller."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from collections.abc import Sequence

from pipetune.runner import MeasureError, MeasureRequest, measure


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="pipetune")
    commands = parser.add_subparsers(dest="command", required=True)
    measure_parser = commands.add_parser("measure")
    measure_parser.add_argument("--target-config", required=True)
    measure_parser.add_argument("--peer-config", required=True)
    measure_parser.add_argument("--output", required=True)
    measure_parser.add_argument(
        "--axio-configure",
        default="build-tools/axio-configure",
    )
    measure_parser.add_argument("--target-binary")
    measure_parser.add_argument("--peer-binary")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    if arguments.command != "measure":
        raise AssertionError(f"unsupported command {arguments.command}")
    request = MeasureRequest(
        target_config=pathlib.Path(arguments.target_config),
        peer_config=pathlib.Path(arguments.peer_config),
        output=pathlib.Path(arguments.output),
        configure_binary=pathlib.Path(arguments.axio_configure),
        target_binary=arguments.target_binary,
        peer_binary=arguments.peer_binary,
    )
    try:
        result = measure(request)
    except MeasureError as error:
        print(f"pipetune measure: {error}", file=sys.stderr)
        return 2
    print(
        json.dumps(
            {"manifest": result.manifest.path, "success": result.success},
            allow_nan=False,
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
