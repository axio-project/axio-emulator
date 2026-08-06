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
    "mem_reg_info",
    "get_name",
    "get_mem_reg",
    "kLocalIpStr",
    "kRemoteIpStr",
    "kLocalMac",
    "kRemoteMac",
    "kDPDK",
    "kRoCE",
    "collect_tx_pkts",
    "tx_flush",
    "rx_burst",
    "dispatch_rx_pkts",
    "pkt_handler_client",
    "pkt_handler_server",
    "get_tx_queue_size",
    "get_rx_queue_size",
    "add_ws_tx_queue",
    "get_ws_tx_queue_size",
    "add_ws_rx_queue",
    "add_rx_rule",
    "get_used_mbuf_num",
    "get_rx_used_desc",
    "ownership_memzone_t",
    "kRemoteMngtIpStr",
    "get_epoch",
    "get_num_qps_available",
    "get_summary",
    "get_qp",
    "free_qp",
    "daemon_reclaim_qps_from_crashed",
    "setup_phy_port",
    "fill_tx_pkts",
    "fill_rx_pkts",
    "get_rx_queue_index",
    "tx_burst_for_arp",
    "is_arp_packet",
    "handle_arp_packet",
    "echo_handler",
    "get_mempool_name",
    "dpdk_strerror",
    "get_mempool",
    "offload_flow_rules",
    "clear_flow_rules",
    "drain_rx_queue",
    "dpdk_mbuf_alloc",
    "dpdk_mbuf_alloc_bulk",
    "dpdk_mbuf_de_alloc",
    "dpdk_mbuf_de_alloc_bulk",
    "dpdk_set_mbuf_paylod",
    "dpdk_extracr_ws_hdr",
    "dpdk_cp_payload",
    "dpdk_mbuf_extract_ws_hdr",
    "dpdk_mbuf_cp_payload",
    "TOTAL_HEADER_LEN",
    "mbuf_eth_hdr",
    "mbuf_ip_hdr",
    "mbuf_tcp_hdr",
    "mbuf_udp_hdr",
    "mbuf_ws_hdr",
    "mbuf_ws_payload",
    "mbuf_ip6_hdr",
    "mbuf_icmp6_hdr",
    "mbuf_push_eth_hdr",
    "mbuf_push_arphdr",
    "mbuf_push_iphdr",
    "mbuf_push_ip6_hdr",
    "mbuf_push_tcphdr",
    "mbuf_push_udphdr",
    "mbuf_push_ws_hdr",
    "mbuf_push_data",
    "mbuf_print",
    "RTE_PKTMBUF_PUSH",
    "routing_info_t",
    "ib_routing_info_t",
    "IBResolve",
    "kRQDepth",
    "kSQDepth",
    "kMemRegionSize",
    "kQKey",
    "kGRHBytes",
    "kMaxDataPerPkt",
    "kDefaultGIDIndex",
    "create_ah",
    "fill_local_routing_info",
    "roce_resolve_phy_port",
    "init_verbs_structs",
    "init_mem_reg_funcs",
    "init_recvs",
    "init_sends",
    "set_local_qp_info",
    "set_remote_qp_info",
    "post_recvs",
    "resolve_pkt_hdr",
    "tx_burst",
    "roce_mbuf_alloc",
    "roce_mbuf_alloc_bulk",
    "roce_mbuf_de_alloc",
    "roce_mbuf_de_alloc_bulk",
    "roce_set_mbuf_paylod",
    "roce_extracr_ws_hdr",
    "roce_cp_payload",
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
