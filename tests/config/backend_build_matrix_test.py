#!/usr/bin/env python3
"""Verify every backend/mempool build choice reaches the generated header."""

import json
import pathlib
import subprocess
import sys
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(*command: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(argument) for argument in command],
        check=False,
        capture_output=True,
        text=True,
    )


def require_success(
    result: subprocess.CompletedProcess[str], label: str
) -> None:
    require(
        result.returncode == 0,
        f"{label} failed ({result.returncode}):\n{result.stdout}{result.stderr}",
    )


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: backend_build_matrix_test.py <axio-configure> <source-root>",
            file=sys.stderr,
        )
        return 2

    configure = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    base_config = source_root / "config/server.toml"
    dpdk_handlers = (
        "ring_mp_mc",
        "ring_sp_sc",
        "ring_mp_sc",
        "ring_sp_mc",
        "ring_mt_rts",
        "ring_mt_hts",
        "stack",
        "lf_stack",
        "bucket",
    )
    cases = [
        ("dpdk", handler, index)
        for index, handler in enumerate(dpdk_handlers)
    ]
    cases.append(("roce", "huge_alloc", 9))

    with tempfile.TemporaryDirectory(prefix="axio-backend-build-matrix-") as root:
        temporary = pathlib.Path(root)
        for backend, handler, handler_id in cases:
            label = f"{backend}-{handler}"
            config = temporary / f"{label}.toml"
            header = temporary / f"{label}.h"
            overrides = {
                "network.backend": backend,
                "knobs.build.mempool_handler": handler,
            }
            materialized = run(
                configure,
                "materialize",
                base_config,
                config,
                "--set-json",
                json.dumps(overrides, sort_keys=True),
            )
            require_success(materialized, f"materialize {label}")
            require_success(run(configure, "validate", config), f"validate {label}")
            require_success(
                run(configure, "generate", config, header), f"generate {label}"
            )

            generated = header.read_text()
            dpdk_mode = 1 if backend == "dpdk" else 0
            roce_mode = 1 if backend == "roce" else 0
            require(
                f"#define AXIO_CONFIG_DPDK_MODE {dpdk_mode}" in generated
                and f"#define AXIO_CONFIG_ROCE_MODE {roce_mode}" in generated,
                f"{label} did not select the requested backend",
            )
            require(
                f"#define AXIO_CONFIG_MEMPOOL_HANDLER {handler_id}" in generated,
                f"{label} did not select the requested mempool handler",
            )

    print("Axio backend build matrix test passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, json.JSONDecodeError) as error:
        print(f"Axio backend build matrix test failed: {error}", file=sys.stderr)
        raise SystemExit(1)
