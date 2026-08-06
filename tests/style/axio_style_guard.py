#!/usr/bin/env python3

"""Reject first-party identifiers retired by the Axio style migration."""

from __future__ import annotations

import re
import sys
from pathlib import Path


SOURCE_SUFFIXES = {".cc", ".h", ".hh"}

# Keep this list explicit. It is extended as each subsystem is migrated so a
# review commit can turn the newly added checks from red to green in one batch.
RETIRED_IDENTIFIERS = (
    "_unused",
    "likely",
    "unlikely",
    "KB",
    "MB",
    "GB",
    "CEIL_2",
    "PERF_TEST",
    "PERF_TEST_LAT",
    "PERF_TEST_THR",
    "PERF_TEST_LAT_MIN_MAX",
    "PERT_TEST_MBUF_RANGE",
    "PERF_LAT_SAMPLE_STRIP",
    "PERF_LAT_SAMPLE_NUM",
    "PERF_LAT_USE_RDTSCP",
    "CLIENT",
    "SERVER",
    "NODE_TYPE",
    "ENABLE_TUNE",
    "ENABLE_AXIO_TEST",
    "msg_handler_type_t",
    "kRxMsgHandler_Empty",
    "kRxMsgHandler_T_APP",
    "kRxMsgHandler_L_APP",
    "kRxMsgHandler_M_APP",
    "kRxMsgHandler_FS_WRITE",
    "kRxMsgHandler_FS_READ",
    "kRxMsgHandler_KV",
    "RoceMode",
    "DpdkMode",
    "RoCE_TYPE",
    "DISPATCHER_TYPE",
    "MEM_REG_TYPE",
    "pkt_handler_type_t",
    "kRxPktHandler_Empty",
    "kRxPktHandler_Echo",
    "kRxMsgHandler",
    "ApplyNewMbuf",
    "kRxPktHandler",
    "EnableInflyMessageLimit",
    "OneStage",
    "FlowSize",
    "kInvaildWorkspaceType",
    "kTxNICType",
    "kTxDispatcherType",
    "kTxApplicationType",
    "kRxNICType",
    "kRxDispatcherType",
    "kRxApplicationType",
    "RESET",
    "RED",
    "GREEN",
    "YELLOW",
    "BLUE",
)

RETIRED_PATTERN = re.compile(
    r"\b(?:" + "|".join(re.escape(name) for name in RETIRED_IDENTIFIERS) + r")\b"
)
RETIRED_MACRO_PATTERN = re.compile(r"^\s*#\s*define\s+(?:UD|RC)\b")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} REPOSITORY_ROOT", file=sys.stderr)
        return 2

    root = Path(sys.argv[1]).resolve()
    violations: list[str] = []
    for path in sorted((root / "src").rglob("*")):
        if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
            continue
        for line_number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), start=1
        ):
            match = RETIRED_PATTERN.search(line)
            macro_match = RETIRED_MACRO_PATTERN.search(line)
            if match or macro_match:
                relative_path = path.relative_to(root)
                violations.append(
                    f"{relative_path}:{line_number}: retired identifier "
                    f"{match.group(0) if match else macro_match.group(0).split()[-1]}"
                )

    if violations:
        print("retired Axio style identifiers found:", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1

    print("Axio style guard passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
