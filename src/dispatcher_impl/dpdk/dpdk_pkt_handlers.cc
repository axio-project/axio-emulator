/**
 * @brief user defined packet handlers for emulation
 */
#include "dpdk_dispatcher.h"

namespace dperf {
  /**
   * @brief packet handler kernel
   */
    size_t DpdkDispatcher::echo_handler() {
      size_t pre_dispatch_total = 0;
      rte_mbuf *mbuf;
      struct eth_hdr *eth = NULL;
      struct iphdr *iph = NULL;

      uint8_t tmp_eth_addr[ETH_ADDR_LEN] = {0};
      uint32_t tmp_ip_addr = 0;

      size_t remain_tx_queue_size = (kNumTxRingEntries - tx_queue_idx_ > rx_queue_idx_) 
                                        ? rx_queue_idx_ : kNumTxRingEntries - tx_queue_idx_;
      for (size_t i = 0; i < remain_tx_queue_size; i++) {
        mbuf = rx_queue_[i];
        eth = mbuf_eth_hdr(mbuf);
        iph = mbuf_ip_hdr(mbuf);

        // swap IP address
        tmp_ip_addr = iph->daddr;
        iph->daddr = iph->saddr;
        iph->saddr = tmp_ip_addr;

        // swap MAC address
        rte_memcpy(tmp_eth_addr, eth->d_addr.bytes, ETH_ADDR_LEN);
        rte_memcpy(eth->d_addr.bytes, eth->s_addr.bytes, ETH_ADDR_LEN);
        rte_memcpy(eth->s_addr.bytes, tmp_eth_addr, ETH_ADDR_LEN);

        // insert packets to tx queue
        tx_queue_[tx_queue_idx_] = mbuf;
        tx_queue_idx_++;

        pre_dispatch_total++;
      }
      for (size_t i = pre_dispatch_total; i < rx_queue_idx_; i++) rte_pktmbuf_free(rx_queue_[i]);
        rx_queue_idx_ = 0;
        return pre_dispatch_total;
    }
  /**
   * @brief packet handler wrapper
   */
  template <pkt_handler_type_t handler>
  size_t DpdkDispatcher::pkt_handler_server() {
    if constexpr (handler == kRxPktHandler_Empty) { return 0; }
    else if (handler == kRxPktHandler_Echo){ return echo_handler(); }
    else {DPERF_ERROR("Invalid packet handler type!"); return 0;} 
  }

// force compile
template size_t DpdkDispatcher::pkt_handler_server<kRxPktHandler>();
} // namespace dperf 