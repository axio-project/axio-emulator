#!/usr/bin/env python3
import pathlib
import subprocess
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    root = pathlib.Path(sys.argv[1]).resolve()
    result = subprocess.run(
        [sys.executable, str(root / "toolchain" / "main.py"), "--help"],
        capture_output=True,
        text=True,
        check=False,
    )
    require(result.returncode == 0, "toolchain --help must not import a deleted tuner")
    require("--tune" not in result.stdout and "--diagnose" not in result.stdout,
            "legacy text-parsing tuning flags must be removed")
    require(not (root / "toolchain" / "tuner.py").exists(),
            "fixed-line tuner implementation must be deleted")
    for source in (root / "toolchain").glob("*.py"):
        contents = source.read_text()
        require("Perf Statistics" not in contents,
                f"{source.name} still parses human performance output")
    print("Axio legacy metrics parser guard passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"Axio legacy metrics parser guard failed: {error}", file=sys.stderr)
        raise SystemExit(1)
