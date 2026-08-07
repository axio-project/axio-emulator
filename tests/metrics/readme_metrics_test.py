#!/usr/bin/env python3

"""Keep the README metrics example aligned with the production contract."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} REPOSITORY_ROOT", file=sys.stderr)
        return 2

    root = Path(sys.argv[1]).resolve()
    readme = (root / "README.md").read_text(encoding="utf-8")
    record_header = (root / "src/metrics/metrics_record.h").read_text(
        encoding="utf-8"
    )
    writer = (root / "src/metrics/metrics_writer.cc").read_text(encoding="utf-8")

    examples = re.findall(r"```json\s*\n(.*?)\n```", readme, re.DOTALL)
    require(examples, "README must contain a JSON metrics example")

    schema_match = re.search(
        r'kMetricsSchema\s*=\s*"([^"]+)"', record_header
    )
    require(schema_match is not None, "production metrics schema is missing")
    production_schema = schema_match.group(1)

    for index, example in enumerate(examples, start=1):
        document = json.loads(example)
        require(
            document.get("schema") == production_schema,
            f"README JSON example {index} uses a stale schema",
        )

        nic_rx = document.get("stages", {}).get("nic_rx", {})
        for field in (
            "throughput_mpps",
            "completion_interval_cycles",
            "completion_interval_ns",
            "slowest_interval_cycles",
            "capacity_interval_cycles",
        ):
            require(field in nic_rx, f"README JSON example {index} lacks {field}")
            require(
                f'\\"{field}\\"' in writer,
                f"README field {field} is absent from the production serializer",
            )

    for config_name in ("client.toml", "server.toml"):
        config = (root / "config" / config_name).read_text(encoding="utf-8")
        require("[metrics]" in config, f"{config_name} lacks [metrics]")
        require("enabled = true" in config, f"{config_name} lacks metrics.enabled")
        require("human_output = true" in config,
                f"{config_name} lacks metrics.human_output")
        require("axio.metrics/v1" in config,
                f"{config_name} does not document the current schema")

    print(f"Axio README metrics examples passed: {len(examples)} JSON document(s)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, json.JSONDecodeError) as error:
        print(f"Axio README metrics examples failed: {error}", file=sys.stderr)
        raise SystemExit(1)
