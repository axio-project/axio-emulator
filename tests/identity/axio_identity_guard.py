#!/usr/bin/env python3

"""Reject legacy DPerf identity in first-party Axio files."""

from __future__ import annotations

import re
import sys
from pathlib import Path


SCAN_PATHS = (
    "README.md",
    "meson.build",
    "meson_options.txt",
    "scan_include.py",
    "scan_src.py",
    "config",
    "scripts",
    "src",
    "tests/smoke",
    "toolchain",
)

EXCLUDED_PATHS = {
    Path("tests/identity/axio_identity_guard.py"),
}

# The paper repository retains its historical name. This URL is attribution,
# not an Axio program identity or user-facing runtime message.
ALLOWED_TEXT = (
    "https://github.com/Huangxy-Minel/Paper-DPerf",
    # DPDK primary/secondary processes use this mempool prefix as an IPC key.
    # Renaming it requires a coordinated protocol migration, not an identity edit.
    "dperf-mp-",
)

LEGACY_PATTERNS = (
    re.compile(r"\bnamespace\s+dperf\b"),
    re.compile(r"\bdperf::"),
    re.compile(r"\bdperf\b"),
    re.compile(r"\bDPERF_[A-Z0-9_]*\b"),
    re.compile(r"\bdPerf\b"),
    re.compile(r"\bDPerf\b"),
)

ROCE_CONSTRUCTOR_PATTERN = re.compile(
    r"RoceDispatcher::RoceDispatcher\([^)]*\)\s*"
    r":\s*Dispatcher\(DispatcherType::kRoce\b",
    re.DOTALL,
)


def iter_files(root: Path):
    for scan_path in SCAN_PATHS:
        path = root / scan_path
        if path.is_file():
            yield path
            continue
        if not path.exists():
            continue
        for candidate in sorted(path.rglob("*")):
            if candidate.is_file() and ".git" not in candidate.parts:
                yield candidate


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} REPOSITORY_ROOT", file=sys.stderr)
        return 2

    root = Path(sys.argv[1]).resolve()
    violations: list[str] = []

    for path in iter_files(root):
        relative_path = path.relative_to(root)
        if relative_path in EXCLUDED_PATHS:
            continue
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            continue

        for line_number, line in enumerate(lines, start=1):
            candidate = line
            for allowed in ALLOWED_TEXT:
                candidate = candidate.replace(allowed, "")
            if any(pattern.search(candidate) for pattern in LEGACY_PATTERNS):
                violations.append(f"{relative_path}:{line_number}: {line.strip()}")

    roce_dispatcher_path = root / "src/dispatcher_impl/roce/roce_dispatcher.cc"
    roce_dispatcher_source = roce_dispatcher_path.read_text(encoding="utf-8")
    if not ROCE_CONSTRUCTOR_PATTERN.search(roce_dispatcher_source):
        violations.append(
            "src/dispatcher_impl/roce/roce_dispatcher.cc: "
            "RoceDispatcher must identify its backend as DispatcherType::kRoce"
        )

    if violations:
        print("Axio identity violations found:", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1

    print("Axio identity guard passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
