#!/usr/bin/env python3
"""Small command-line utilities for Axio metrics JSONL files."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from typing import Sequence


class MetricsFormatError(ValueError):
    pass


def reject_nonfinite(value: str) -> None:
    raise MetricsFormatError(f"non-finite JSON constant {value}")


def load_jsonl(path: pathlib.Path) -> list[str]:
    records = []
    with path.open(encoding="utf-8") as source:
        for line_number, raw_line in enumerate(source, start=1):
            line = raw_line.rstrip("\n")
            if not line:
                raise MetricsFormatError(f"{path}:{line_number}: blank line")
            try:
                record = json.loads(line, parse_constant=reject_nonfinite)
            except (json.JSONDecodeError, MetricsFormatError) as error:
                raise MetricsFormatError(
                    f"{path}:{line_number}: invalid JSON: {error}"
                ) from error
            if not isinstance(record, dict):
                raise MetricsFormatError(
                    f"{path}:{line_number}: expected a JSON object"
                )
            records.append(line)
    if not records:
        raise MetricsFormatError(f"{path}: no metrics records")
    return records


def pretty_json(source: str, indent_width: int = 2) -> str:
    output = []
    depth = 0
    in_string = False
    escaped = False
    for character in source:
        if in_string:
            output.append(character)
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            continue

        if character == '"':
            in_string = True
            output.append(character)
        elif character in "{[":
            depth += 1
            output.append(character)
            output.append("\n" + " " * (depth * indent_width))
        elif character in "}]":
            depth -= 1
            output.append("\n" + " " * (depth * indent_width) + character)
        elif character == ",":
            output.append(",\n" + " " * (depth * indent_width))
        elif character == ":":
            output.append(": ")
        elif not character.isspace():
            output.append(character)
    return "".join(output)


def indent_block(value: str, width: int) -> str:
    prefix = " " * width
    return "\n".join(prefix + line for line in value.splitlines())


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Inspect Axio metrics JSONL")
    commands = parser.add_subparsers(dest="command", required=True)
    pretty = commands.add_parser("pretty", help="pretty-print metrics JSONL")
    pretty.add_argument(
        "--array",
        action="store_true",
        help="emit one standard JSON array instead of a stream of objects",
    )
    pretty.add_argument("path", type=pathlib.Path)
    return parser


def main(arguments: Sequence[str]) -> int:
    parsed = build_parser().parse_args(arguments)
    try:
        records = load_jsonl(parsed.path)
    except (MetricsFormatError, OSError) as error:
        print(f"axio-metrics: {error}", file=sys.stderr)
        return 1

    if parsed.array:
        body = ",\n".join(indent_block(pretty_json(record), 2)
                           for record in records)
        print("[\n" + body + "\n]")
    else:
        print("\n".join(pretty_json(record) for record in records))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
