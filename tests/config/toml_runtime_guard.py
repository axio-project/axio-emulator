#!/usr/bin/env python3
"""Keep the emulator runtime on the typed TOML configuration path."""

import pathlib
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: toml_runtime_guard.py <source-root>", file=sys.stderr)
        return 2

    source_root = pathlib.Path(sys.argv[1])
    runtime_sources = {
        path: (source_root / path).read_text()
        for path in ("src/main.cc", "src/config.h", "src/config.cc")
    }

    forbidden = (
        "config_values_",
        "UserConfig(const std::string& filename)",
        "_configure_workload",
        "_configure_server",
        'config/send_config',
        'config/recv_config',
    )
    for token in forbidden:
        owners = [path for path, contents in runtime_sources.items() if token in contents]
        require(not owners, f"legacy runtime config token {token!r} remains in {owners}")

    main_source = runtime_sources["src/main.cc"]
    require('"--config"' in main_source, "axio must require an explicit TOML config")
    require(
        "config::load_config" in main_source,
        "axio must load runtime configuration through the typed TOML parser",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
