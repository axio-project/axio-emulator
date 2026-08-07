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

    examples = re.findall(r"```json\s*\n(.*?)\n```", readme, re.DOTALL)
    require(examples, "README must contain a JSON metrics example")

    for index, example in enumerate(examples, start=1):
        document = json.loads(example)
        require(set(document) == {
            "window_id", "throughput", "latency", "stages", "counters",
        }, f"README JSON example {index} has the wrong top-level fields")
        require(set(document["throughput"]) == {"e2e_mpps"},
                f"README JSON example {index} has extra throughput fields")
        require(set(document["latency"]) == {"p50_us", "p99_us", "p999_us"},
                f"README JSON example {index} has extra latency fields")
        require(set(document["stages"]) == {"app_tx", "nic_rx"},
                f"README JSON example {index} has extra stages")
        require(set(document["stages"]["app_tx"]) == {
            "completion_time_per_packet_us", "stall_time_per_packet_us",
        }, f"README JSON example {index} has extra app_tx fields")
        require(set(document["stages"]["nic_rx"]) == {
            "throughput_mpps",
            "completion_interval_cycles",
            "completion_interval_ns",
            "slowest_interval_cycles",
            "capacity_interval_cycles",
        }, f"README JSON example {index} has the wrong nic_rx fields")
        require(set(document["counters"]) == {
            "app_enqueue_drop_count", "dispatcher_enqueue_drop_count",
            "nic_rx_completion_error_count",
        }, f"README JSON example {index} has extra counter fields")

    for config_name in ("client.toml", "server.toml"):
        config = (root / "config" / config_name).read_text(encoding="utf-8")
        require("[metrics]" in config, f"{config_name} lacks [metrics]")
        require("enabled = true" in config, f"{config_name} lacks metrics.enabled")
        require("human_output = true" in config,
                f"{config_name} lacks metrics.human_output")
        require("compact JSON record" in config,
                f"{config_name} does not document compact metrics")

    print(f"Axio README metrics examples passed: {len(examples)} JSON document(s)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, json.JSONDecodeError) as error:
        print(f"Axio README metrics examples failed: {error}", file=sys.stderr)
        raise SystemExit(1)
