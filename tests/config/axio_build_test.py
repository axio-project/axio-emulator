#!/usr/bin/env python3
"""Contract tests for the two-phase Axio Ninja build driver."""

import json
import os
import pathlib
import subprocess
import sys
import tempfile


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def write_build_options(build_dir: pathlib.Path, config: str) -> None:
    info_dir = build_dir / "meson-info"
    info_dir.mkdir(parents=True)
    (build_dir / "build.ninja").write_text("# fake ninja graph\n")
    (info_dir / "intro-buildoptions.json").write_text(
        json.dumps([{"name": "axio_config", "value": config}])
    )


def run_driver(
    driver: pathlib.Path,
    ninja: pathlib.Path,
    build_dir: pathlib.Path,
    log: pathlib.Path,
    fail_on_generated: bool = False,
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["AXIO_BUILD_TEST_LOG"] = str(log)
    environment["AXIO_BUILD_TEST_FAIL_GENERATED"] = (
        "1" if fail_on_generated else "0"
    )
    return subprocess.run(
        [
            sys.executable,
            str(driver),
            str(build_dir),
            "--target",
            "axio",
            "--jobs",
            "3",
            "--ninja",
            str(ninja),
        ],
        check=False,
        capture_output=True,
        text=True,
        env=environment,
    )


def calls(log: pathlib.Path) -> list[list[str]]:
    if not log.exists():
        return []
    return [json.loads(line) for line in log.read_text().splitlines()]


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: axio_build_test.py <source-root>", file=sys.stderr)
        return 2

    source_root = pathlib.Path(sys.argv[1])
    driver = source_root / "toolchain/axio_build.py"
    with tempfile.TemporaryDirectory(prefix="axio-build-test-") as temp_dir:
        temp = pathlib.Path(temp_dir)
        ninja = temp / "fake-ninja.py"
        ninja.write_text(
            """#!/usr/bin/env python3
import json
import os
import pathlib
import sys

log = pathlib.Path(os.environ["AXIO_BUILD_TEST_LOG"])
with log.open("a") as output:
    output.write(json.dumps(sys.argv[1:]) + "\\n")
if (os.environ.get("AXIO_BUILD_TEST_FAIL_GENERATED") == "1" and
        "generated/axio_config_generated.h" in sys.argv):
    raise SystemExit(7)
"""
        )
        ninja.chmod(0o755)

        generated_build = temp / "generated-build"
        write_build_options(generated_build, "/tmp/client.toml")
        generated_log = temp / "generated.log"
        generated = run_driver(
            driver, ninja, generated_build, generated_log
        )
        require(generated.returncode == 0, generated.stderr)
        require(
            calls(generated_log)
            == [
                [
                    "-C",
                    str(generated_build.resolve()),
                    "generated/axio_config_generated.h",
                ],
                ["-C", str(generated_build.resolve()), "-j", "3", "axio"],
            ],
            "generated build must update the header before compiling Axio",
        )

        fallback_build = temp / "fallback-build"
        write_build_options(fallback_build, "")
        fallback_log = temp / "fallback.log"
        fallback = run_driver(driver, ninja, fallback_build, fallback_log)
        require(fallback.returncode == 0, fallback.stderr)
        require(
            calls(fallback_log)
            == [["-C", str(fallback_build.resolve()), "-j", "3", "axio"]],
            "fallback build must invoke Ninja once",
        )

        failed_log = temp / "failed.log"
        failed = run_driver(
            driver,
            ninja,
            generated_build,
            failed_log,
            fail_on_generated=True,
        )
        require(failed.returncode == 7, "header-generation failure was hidden")
        require(len(calls(failed_log)) == 1, "compile ran after header failure")

    print("Axio build driver test passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError, json.JSONDecodeError) as error:
        print(f"Axio build driver test failed: {error}", file=sys.stderr)
        raise SystemExit(1)
