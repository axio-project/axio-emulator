#!/usr/bin/env python3
"""
Generate recv_config / send_config workload line for Plan-A test.
Each "ws" here means one dispatcher (which owns 1 DPDK QP).

Usage:
  ./gen_plan_a.py --total 32 --tpl config/recv_config --out config/recv_config
Will rewrite the line starting with `workload :` to launch T dispatcher ws
(ws_id 0..T-1), with one app ws sharing each ws_id (DISPATCHER|WORKER mode).
"""
import argparse
import sys
import re
from pathlib import Path


def build_workload_line(total: int, workload_type: int = 1) -> str:
    ids_csv = ",".join(str(i) for i in range(total))
    ids_pipe = "|".join(str(i) for i in range(total))
    # remote dispatcher list -> use 0..total-1 too, peer side will mirror
    remote = ids_csv
    return (
        f"workload : {workload_type} : "
        "RXNIC,RXDispatcher,RxApplication,TxDispatcher,TxNIC : "
        f"{remote} : {ids_pipe} : {ids_pipe}"
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--total", type=int, required=True,
                    help="Total dispatcher ws (= total QPs on this side).")
    ap.add_argument("--tpl", type=Path, required=True,
                    help="Template config file.")
    ap.add_argument("--out", type=Path, required=True,
                    help="Output config file.")
    ap.add_argument("--disp-queue-num", type=int, default=None,
                    help="Override kDispQueueNum (must be >= total).")
    args = ap.parse_args()

    text = args.tpl.read_text().splitlines()
    new_line = build_workload_line(args.total)

    out_lines = []
    replaced = False
    for ln in text:
        stripped = ln.lstrip()
        if (not replaced) and stripped.startswith("workload ") and not stripped.startswith("#"):
            out_lines.append(new_line)
            replaced = True
            continue
        if args.disp_queue_num is not None and stripped.startswith("kDispQueueNum"):
            out_lines.append(f"kDispQueueNum           : {args.disp_queue_num}")
            continue
        out_lines.append(ln)
    if not replaced:
        print("ERROR: no active 'workload :' line in template", file=sys.stderr)
        sys.exit(1)
    args.out.write_text("\n".join(out_lines) + "\n")
    print(f"Wrote {args.out} (total={args.total})")


if __name__ == "__main__":
    main()
