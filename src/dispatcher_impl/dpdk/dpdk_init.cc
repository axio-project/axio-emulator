/**
 * @file dpdk_init.cc
 * @brief Initialization code for a DPDK port. This is a separate file because
 * it's used by both the DPerf library and the DPDK QP management daemon.
 */

#include "dpdk_externs.h"
#include "dpdk_dispatcher.h"

namespace dperf {

void DpdkDispatcher::setup_phy_port(uint16_t phy_port, size_t numa_node,
                                   DpdkProcType proc_type, uint8_t enabled_queue_num, size_t tx_batch, size_t rx_batch) {
  _unused(proc_type);
  uint16_t num_ports = rte_eth_dev_count_avail();
  if (phy_port >= num_ports) {
    fprintf(stderr,
            "Error: Port %u (0-based) requested, but only %u DPDK ports "
            "available. Please ensure:\n",
            phy_port, num_ports);
    fprintf(stderr,
            "1. If you have a DPDK-capable port, ensure that (a) the NIC's "
            "NUMA node has huge pages, and (b) this process is not pinned "
            "(e.g., via numactl) to a different NUMA node than the NIC's.\n");

    const char *ld_library_path = getenv("LD_LIBRARY_PATH");
    const char *library_path = getenv("LIBRARY_PATH");

    fprintf(stderr,
            "2. Your LD_LIBRARY_PATH (= %s) and/or LIBRARY_PATH (= %s) "
            "contains the NIC's userspace libraries (e.g., libmlx5.so).\n",
            ld_library_path == nullptr ? "not set" : ld_library_path,
            library_path == nullptr ? "not set" : library_path);
    rt_assert(false);
  }

  rte_eth_dev_info dev_info;
  rte_eth_dev_info_get(phy_port, &dev_info);
  printf("Max RX queues: %u, Max TX queues: %u\n", dev_info.max_rx_queues,
         dev_info.max_tx_queues);
  rt_assert(dev_info.rx_desc_lim.nb_max >= kNumRxRingEntries,
            "Device RX ring too small");
  rt_assert(dev_info.tx_desc_lim.nb_max >= kNumTxRingEntries,
            "Device TX ring too small");
  DPERF_INFO("Initializing port %u with driver %s\n", phy_port,
            dev_info.driver_name);

  // Create per-thread RX and TX queues
  rte_eth_conf eth_conf;
  memset(&eth_conf, 0, sizeof(eth_conf));

  eth_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

  eth_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
  eth_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;

  int ret = rte_eth_dev_configure(phy_port, enabled_queue_num,
                                  enabled_queue_num, &eth_conf);
  rt_assert(ret == 0, "Ethdev configuration error: ", strerror(-1 * ret));

  // Set up all RX and TX queues and start the device. This can't be done later
  // on a per-thread basis since we must start the device to use any queue.
  // Once the device is started, more queues cannot be added without stopping
  // and reconfiguring the device.
  for (size_t i = 0; i < enabled_queue_num; i++) {
    const std::string pname = get_mempool_name(phy_port, i);
    rte_mempool *mempool =
    #if NODE_TYPE == CLIENT
      rte_pktmbuf_pool_create(pname.c_str(), kDpdkMempoolSize, RTE_MEMPOOL_CACHE_MAX_SIZE/* cache */, 0 /* priv size */, kMbufSize, numa_node);
    #else
      rte_pktmbuf_pool_create(pname.c_str(), kDpdkMempoolSize, 0/* cache */, 0 /* priv size */, kMbufSize, numa_node);
    #endif
    rt_assert(mempool != nullptr, "Mempool create failed: " + dpdk_strerror());

    rte_eth_rxconf eth_rx_conf;
    memset(&eth_rx_conf, 0, sizeof(eth_rx_conf));
    eth_rx_conf.rx_thresh.pthresh = rx_batch;   // Typically, 16 are sufficient to utilize the full PCIe bandwidth.

    int ret = rte_eth_rx_queue_setup(phy_port, i, kNumRxRingEntries, numa_node,
                                     &eth_rx_conf, mempool);
    rt_assert(ret == 0, "Failed to setup RX queue: " + std::to_string(i) +
                            ". Error " + strerror(-1 * ret));

    rte_eth_txconf eth_tx_conf;
    memset(&eth_tx_conf, 0, sizeof(eth_tx_conf));
    eth_tx_conf.tx_thresh.pthresh = tx_batch;
    eth_tx_conf.offloads = eth_conf.txmode.offloads;

    ret = rte_eth_tx_queue_setup(phy_port, i, kNumTxRingEntries, numa_node,
                                 &eth_tx_conf);
    // ret = rte_eth_tx_queue_setup(phy_port, i, kNumTxRingEntries, numa_node,
    //                              NULL);
    rt_assert(ret == 0, "Failed to setup TX queue: " + std::to_string(i));
  }

  rte_eth_dev_start(phy_port);
}

}  // namespace dperf


