#!/usr/bin/env python3
"""Regression test for canonical addresses in the legacy tuner config."""

import pathlib
import sys
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: legacy_config_parser_test.py <source-root>",
              file=sys.stderr)
        return 2

    source_root = pathlib.Path(sys.argv[1])
    sys.path.insert(0, str(source_root / "toolchain"))
    from config_parser import Config

    with tempfile.TemporaryDirectory(prefix="axio-legacy-address-") as temp_dir:
        source = source_root / "config/send_config"
        config_path = pathlib.Path(temp_dir) / "send_config"
        config_path.write_text(source.read_text())

        config = Config(str(config_path))
        require(config.local_mac == "10:70:fd:6b:93:5c",
                "legacy parser did not preserve canonical MAC")
        require(config.remote_mac == "10:70:fd:87:0e:ba",
                "legacy parser did not preserve peer MAC")
        require(config.device_pcie == "0000:98:00.0",
                "legacy parser did not preserve canonical PCIe BDF")
        require(config.device_name == "rocep152s0f0",
                "legacy parser did not preserve device name")

        config.write_back()
        output = pathlib.Path(str(config_path) + ".out").read_text()
        require("local_mac : 10:70:fd:6b:93:5c" in output,
                "legacy writer changed canonical MAC")
        require("device_pcie : 0000:98:00.0" in output,
                "legacy writer changed canonical PCIe BDF")
        require("device_name : rocep152s0f0" in output,
                "legacy writer dropped device name")

    print("Axio legacy config parser test passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, ValueError) as error:
        print(f"Axio legacy config parser test failed: {error}",
              file=sys.stderr)
        raise SystemExit(1)
