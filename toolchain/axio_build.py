#!/usr/bin/env python3
"""Build Axio after stabilizing its generated configuration header."""

import argparse
import json
import pathlib
import subprocess
import sys
from typing import Sequence


GENERATED_HEADER_TARGET = "generated/axio_config_generated.h"


def parse_arguments(arguments: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Update the generated Axio config header, then compile the target "
            "from a stable Ninja graph."
        )
    )
    parser.add_argument("build_dir", type=pathlib.Path)
    parser.add_argument("--target", default="axio")
    parser.add_argument("--jobs", type=int)
    parser.add_argument("--ninja", default="ninja")
    parsed = parser.parse_args(arguments)
    if parsed.jobs is not None and parsed.jobs < 1:
        parser.error("--jobs must be positive")
    return parsed


def axio_config_path(build_dir: pathlib.Path) -> str:
    options_path = build_dir / "meson-info/intro-buildoptions.json"
    with options_path.open() as options_file:
        options = json.load(options_file)
    for option in options:
        if option.get("name") == "axio_config":
            value = option.get("value")
            if not isinstance(value, str):
                raise ValueError("Meson axio_config option is not a string")
            return value
    raise ValueError("Meson build does not define the axio_config option")


def run_ninja(
    ninja: str, build_dir: pathlib.Path, arguments: Sequence[str]
) -> int:
    result = subprocess.run(
        [ninja, "-C", str(build_dir), *arguments], check=False
    )
    return result.returncode


def main(arguments: Sequence[str]) -> int:
    parsed = parse_arguments(arguments)
    build_dir = parsed.build_dir.resolve()
    if not (build_dir / "build.ninja").is_file():
        print(
            f"Axio build directory is not configured: {build_dir}",
            file=sys.stderr,
        )
        return 2

    try:
        config_path = axio_config_path(build_dir)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(
            f"Failed to inspect Axio Meson configuration: {error}",
            file=sys.stderr,
        )
        return 2

    if config_path:
        status = run_ninja(parsed.ninja, build_dir, [GENERATED_HEADER_TARGET])
        if status != 0:
            return status

    compile_arguments = []
    if parsed.jobs is not None:
        compile_arguments.extend(["-j", str(parsed.jobs)])
    compile_arguments.append(parsed.target)
    return run_ninja(parsed.ninja, build_dir, compile_arguments)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
