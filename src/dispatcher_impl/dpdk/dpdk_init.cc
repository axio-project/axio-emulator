/**
 * @file dpdk_init.cc
 * @brief Initialization code for a DPDK port. This is a separate file because
 * it's used by both the Axio library and the DPDK QP management daemon.
 */

#include "axio/config/backend_capabilities.h"
#include "dpdk_dispatcher.h"
#include "dpdk_externs.h"

namespace axio {

#if AXIO_DPDK_MODE
static_assert(AXIO_CONFIG_MEMPOOL_HANDLER >= 0 &&
                  AXIO_CONFIG_MEMPOOL_HANDLER <
                  AXIO_MEMPOOL_HANDLER_HUGE_ALLOC,
              "DPDK requires a registered DPDK mempool handler");
#endif

void DpdkDispatcher::setup_physical_port(
    uint16_t physical_port, size_t numa_node, DpdkProcType process_type,
    uint8_t enabled_queue_count) {
  AXIO_UNUSED(process_type);
  uint16_t num_ports = rte_eth_dev_count_avail();
  if (physical_port >= num_ports) {
    fprintf(stderr,
            "Error: Port %u (0-based) requested, but only %u DPDK ports "
            "available. Please ensure:\n",
            physical_port, num_ports);
    fprintf(stderr,
            "1. If you have a DPDK-capable port, ensure that (a) the NIC's "
            "NUMA node has huge pages, and (b) this process is not pinned "
            "(e.g., via numactl) to a different NUMA node than the NIC's.\n");

    const char* ld_library_path = getenv("LD_LIBRARY_PATH");
    const char* library_path = getenv("LIBRARY_PATH");

    fprintf(stderr,
            "2. Your LD_LIBRARY_PATH (= %s) and/or LIBRARY_PATH (= %s) "
            "contains the NIC's userspace libraries (e.g., libmlx5.so).\n",
            ld_library_path == nullptr ? "not set" : ld_library_path,
            library_path == nullptr ? "not set" : library_path);
    rt_assert(false);
  }

  rte_eth_dev_info dev_info;
  int ret = rte_eth_dev_info_get(physical_port, &dev_info);
  rt_assert(ret == 0, "Failed to query DPDK port capabilities: ",
            strerror(-1 * ret));
  printf("Max RX queues: %u, Max TX queues: %u\n", dev_info.max_rx_queues,
         dev_info.max_tx_queues);
  rt_assert(dev_info.rx_desc_lim.nb_max >= kNumRxRingEntries,
            "Device RX ring too small");
  rt_assert(dev_info.tx_desc_lim.nb_max >= kNumTxRingEntries,
            "Device TX ring too small");
  rt_assert(kMtu >= dev_info.min_mtu && kMtu <= dev_info.max_mtu,
            "Configured MTU " + std::to_string(kMtu) +
                " is outside DPDK port range [" +
                std::to_string(dev_info.min_mtu) + ", " +
                std::to_string(dev_info.max_mtu) + "]");
  AXIO_INFO("Initializing port %u with driver %s\n", physical_port,
            dev_info.driver_name);

  // Create per-thread RX and TX queues
  rte_eth_conf eth_conf;
  memset(&eth_conf, 0, sizeof(eth_conf));

  eth_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

  eth_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
  eth_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;

  ret = rte_eth_dev_configure(physical_port, enabled_queue_count,
                              enabled_queue_count, &eth_conf);
  rt_assert(ret == 0, "Ethdev configuration error: ", strerror(-1 * ret));

  ret = rte_eth_dev_set_mtu(physical_port, static_cast<uint16_t>(kMtu));
  rt_assert(ret == 0,
            "Failed to set DPDK port MTU to " + std::to_string(kMtu) +
                ": " + strerror(-1 * ret));

  // Set up all RX and TX queues and start the device. This can't be done later
  // on a per-thread basis since we must start the device to use any queue.
  // Once the device is started, more queues cannot be added without stopping
  // and reconfiguring the device.
  for (size_t i = 0; i < enabled_queue_count; i++) {
    const std::string mempool_name =
        DpdkDispatcher::_mempool_name(physical_port, i);
    const std::string mempool_ops_name(config::dpdk_mempool_ops_name(
        static_cast<config::MempoolHandler>(AXIO_CONFIG_MEMPOOL_HANDLER)));
    rte_mempool* mempool = rte_pktmbuf_pool_create_by_ops(
        mempool_name.c_str(), kDpdkMempoolSize,
        AXIO_CONFIG_MEMPOOL_CACHE_SIZE, 0, kMbufSize, numa_node,
        mempool_ops_name.c_str());
    rt_assert(
        mempool != nullptr,
        "Mempool create failed with DPDK ops '" +
            mempool_ops_name + "': " + DpdkDispatcher::_error_string());

    rte_eth_rxconf eth_rx_conf;
    memset(&eth_rx_conf, 0, sizeof(eth_rx_conf));
    // pthresh is an 8-bit device-prefetch hint, not Axio's NIC post-size
    // contract. Keep the historical safe default while the datapath uses
    // nic_rx_post_size as the actual rte_eth_rx_burst limit.
    eth_rx_conf.rx_thresh.pthresh = 32;

    ret = rte_eth_rx_queue_setup(physical_port, i, kNumRxRingEntries,
                                 numa_node, &eth_rx_conf, mempool);
    rt_assert(ret == 0, "Failed to setup RX queue: " + std::to_string(i) +
                            ". Error " + strerror(-1 * ret));

    rte_eth_txconf eth_tx_conf;
    memset(&eth_tx_conf, 0, sizeof(eth_tx_conf));
    // nic_tx_post_size caps rte_eth_tx_burst; it must not be truncated into
    // the unrelated 8-bit pthresh field.
    eth_tx_conf.tx_thresh.pthresh = 16;
    eth_tx_conf.offloads = eth_conf.txmode.offloads;

    ret = rte_eth_tx_queue_setup(physical_port, i, kNumTxRingEntries, numa_node,
                                 &eth_tx_conf);
    // ret = rte_eth_tx_queue_setup(physical_port, i, kNumTxRingEntries, numa_node,
    //                              nullptr);
    rt_assert(ret == 0, "Failed to setup TX queue: " + std::to_string(i));
  }

  rte_eth_dev_start(physical_port);
}

}  // namespace axio
