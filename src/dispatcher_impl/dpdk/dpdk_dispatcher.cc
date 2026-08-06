#include "dpdk_dispatcher.h"
#include "dpdk_externs.h"

namespace axio {
/**
 * ----------------------DpdkDispatcher methods----------------------
 */ 
DpdkDispatcher::DpdkDispatcher(uint8_t ws_id, uint8_t phy_port,
                               size_t numa_node, UserConfig* user_config)
    : Dispatcher(
          DispatcherType::kDpdk, ws_id, phy_port, numa_node, user_config) {
  // The first thread to grab the lock initializes DPDK (as DPDK daemon process)
  g_dpdk_lock.lock();
  rte_thread_register();    // Register this thread with as an EAL thread to enable mempool cache
  if (g_dpdk_initialized) {
    AXIO_INFO("DPDK dispatcher for Workspace %u is skipping DPDK EAL initialization.\n", ws_id);
    this->process_type_ = ((rte_eal_process_type() == RTE_PROC_PRIMARY)
                        ? DpdkProcType::kPrimary
                        : DpdkProcType::kSecondary);
  } else {
    AXIO_INFO("DPDK dispatcher for Workspace %u is initializing DPDK EAL.\n", ws_id);
    // clang-format off
    const char *rte_argv[] = {
        "-c",            "0x0",
        "-n",            "8",  // Memory channels
        "-m",            "1024", // Max memory in megabytes
        "-a",            user_config->server().device_pcie_address_,
        "--proc-type",   "auto",
        "--log-level",   (AXIO_LOG_LEVEL >= AXIO_LOG_LEVEL_INFO) ? "8" : "0",
        nullptr};
    // clang-format on
    const int rte_argc =
        static_cast<int>(sizeof(rte_argv) / sizeof(rte_argv[0])) - 1;
    int ret = rte_eal_init(rte_argc, const_cast<char **>(rte_argv));
    rt_assert(ret >= 0, "Failed to initialize DPDK");
    this->process_type_ = ((rte_eal_process_type() == RTE_PROC_PRIMARY)
                            ? DpdkProcType::kPrimary
                            : DpdkProcType::kSecondary);
    // Create a fake memzone
    g_memzone = new OwnershipMemzone();
    g_memzone->init();
    g_dpdk_initialized = true;
  }
  // Get an available queue on phy_port
  this->queue_pair_id_ = g_memzone->acquire_queue_pair(phy_port, 33 /* XXX */);
  if (this->queue_pair_id_ != kInvalidQpId) {
    AXIO_INFO("DPDK dispatcher for Ws %u got QP %zu\n", ws_id, this->queue_pair_id_);
  } else {
    AXIO_ERROR(
        "DPDK dispatcher for Ws %u failed to get a free TX/RQ queue pair. "
        "All %zu available queue pairs are in use by Workspace objects.\n",
        ws_id, kMaxQueuesPerPort);
    throw std::runtime_error("Failed to get DPDK QP");
  }
  // Init mempool
  const std::string mempool_name =
      this->_mempool_name(phy_port, this->queue_pair_id_);
  if (this->process_type_ == DpdkProcType::kSecondary) {
    // The Axio DPDK management daemon has already initialized phy_port
    this->mempool_ = rte_mempool_lookup(mempool_name.c_str());
    rt_assert(this->mempool_ != nullptr,
            std::string("Failed to find Axio DPDK daemon's mempool ") +
                mempool_name.c_str());
    this->_drain_rx_queue();

    const size_t n_avail = rte_mempool_avail_count(this->mempool_);
    if (n_avail < kDpdkMempoolSize) {
      AXIO_WARN(
          "DPDK dispatcher for Ws %u: Mempool has only %zu free mbufs "
          "out of %zu. %zu mbufs have been leaked by previous processes that "
          "owned this mempool.\n",
          ws_id, n_avail, kDpdkMempoolSize, (kDpdkMempoolSize - n_avail));
    }
  } else {
    if (!g_port_initialized[phy_port]) {
      g_port_initialized[phy_port] = true;
      DpdkDispatcher::setup_physical_port(
          phy_port, numa_node, DpdkProcType::kPrimary,
          user_config->tunables().dispatcher_queue_count_,
          user_config->tunables().nic_tx_post_size_,
          user_config->tunables().nic_rx_post_size_);
    }

    this->mempool_ = rte_mempool_lookup(mempool_name.c_str());
    rt_assert(
        this->mempool_ != nullptr,
        std::string("Failed to find self's mempool ") + mempool_name.c_str());
  }
  g_dpdk_lock.unlock();

  this->_resolve_physical_port();
  this->destination_mac_ = new EthernetAddress;
  memcpy(this->destination_mac_, &this->remote_mac(), sizeof(EthernetAddress));
  this->destination_ip_ = new IpAddress;
  parse_ip_address(this->destination_ip_, this->remote_ip());
  this->_initialize_memory_region_functions();

  // init rte_flow
  this->_offload_flow_rules(
      ws_id, numa_node, phy_port, this->queue_pair_id_);
#if AXIO_ENABLE_TESTS
  /// create management TCP connection (this is not necessary for DPDK, but for consistency with other projects, e.g., axio-bf3-express
  //// In DPDK, the port has been occupied by the DPDK process, so we can't use the port for management connection
  //// Use mngt NIC for management connection, while using the port for data transfer
  QueuePairInfo local_queue_pair_info;
  QueuePairInfo remote_queue_pair_info;
  local_queue_pair_info.queue_pair_number_ = this->queue_pair_id_;
  memcpy(local_queue_pair_info.mac_address_, this->resolve_.mac_addr_.bytes_,
         sizeof(this->resolve_.mac_addr_.bytes_));
  local_queue_pair_info.mtu_ = kMTU;
  local_queue_pair_info.initialized_ = true;
#if AXIO_NODE_TYPE == AXIO_SERVER
  TcpServer management_server(kDefaultMngtPort + ws_id);
  management_server.accept_connection();
  management_server.send_message(local_queue_pair_info.serialize());
  remote_queue_pair_info.deserialize(management_server.receive_message());
  management_server.disconnect();
#elif AXIO_NODE_TYPE == AXIO_CLIENT
  TcpClient management_client;
  management_client.connect_to_server(this->remote_management_ip_,
                                      kDefaultMngtPort + ws_id);
  management_client.send_message(local_queue_pair_info.serialize());
  remote_queue_pair_info.deserialize(management_client.receive_message());
  management_client.disconnect();
#endif

  rt_assert(remote_queue_pair_info.mtu_ == kMTU, "MTU mismatch");
#endif

  AXIO_WARN(
      "DpdkDispatcher created for Workspace ID %u, queue %zu\n",
      ws_id, this->queue_pair_id_);
}

DpdkDispatcher::~DpdkDispatcher() {
  AXIO_INFO("Destroying dispatcher for ID %lu\n", this->queue_pair_id_);
  this->_drain_rx_queue();

  int ret = g_memzone->release_queue_pair(
      this->physical_port(), this->queue_pair_id_);
  rt_assert(ret == 0, "Failed to free QP\n");

  this->_clear_flow_rules(this->physical_port());
}

void DpdkDispatcher::_clear_flow_rules(uint8_t port_id) {
  struct rte_flow_error error;

  const int result = rte_flow_destroy(port_id, this->flow_, &error);
  if (result != 0) {
    AXIO_ERROR("Failed to destroy flow rule: %s\n", error.message);
  }
}

void DpdkDispatcher::_offload_flow_rules(
    uint8_t workspace_id, uint8_t numa_node, uint8_t port_id,
    uint64_t queue_pair_id) {
  static constexpr size_t kMaxPatternCount = 3;
  static constexpr size_t kMaxActionCount = 2;

  int result;
  rte_flow_attr attributes;
  rte_flow_item pattern[kMaxPatternCount];
  rte_flow_item arp_pattern[kMaxPatternCount];
  rte_flow_item drop_pattern[kMaxPatternCount];
  rte_flow_action actions[kMaxActionCount];
  rte_flow_action arp_actions[kMaxActionCount];
  rte_flow_action drop_actions[kMaxActionCount];
  rte_flow_action_queue queue_action;
  rte_flow_action_queue arp_queue_action;
  rte_flow_item_udp udp_spec;
  rte_flow_item_udp udp_mask;
  rte_flow_error error;
  rte_flow_item_eth ethernet_spec;
  rte_flow_item_eth ethernet_mask;
  rte_flow_item_eth arp_spec;
  rte_flow_item_eth arp_mask;

  memset(pattern, 0, sizeof(pattern));
  memset(actions, 0, sizeof(actions));
  memset(arp_pattern, 0, sizeof(arp_pattern));
  memset(arp_actions, 0, sizeof(arp_actions));
  memset(drop_pattern, 0, sizeof(drop_pattern));
  memset(drop_actions, 0, sizeof(drop_actions));
  memset(&attributes, 0, sizeof(attributes));
  attributes.ingress = 1;

  memset(&queue_action, 0, sizeof(queue_action));
  queue_action.index = queue_pair_id;
  actions[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  actions[0].conf = &queue_action;
  actions[1].type = RTE_FLOW_ACTION_TYPE_END;

  memset(&udp_spec, 0, sizeof(udp_spec));
  memset(&udp_mask, 0, sizeof(udp_mask));
  udp_spec.hdr.dst_port =
      htons(static_cast<uint32_t>(workspace_id) + kDefaultUdpPort);
  udp_mask.hdr.dst_port = 0xffff;
  udp_mask.hdr.src_port = 0x0000;
  pattern[0].type = RTE_FLOW_ITEM_TYPE_IPV4;
  pattern[1].type = RTE_FLOW_ITEM_TYPE_UDP;
  pattern[1].spec = &udp_spec;
  pattern[1].mask = &udp_mask;
  pattern[2].type = RTE_FLOW_ITEM_TYPE_END;

  memset(&arp_queue_action, 0, sizeof(arp_queue_action));
  arp_queue_action.index = 0;
  arp_actions[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
  arp_actions[0].conf = &arp_queue_action;
  arp_actions[1].type = RTE_FLOW_ACTION_TYPE_END;

  memset(&arp_spec, 0, sizeof(arp_spec));
  memset(&arp_mask, 0, sizeof(arp_mask));
  arp_spec.hdr.ether_type = RTE_BE16(RTE_ETHER_TYPE_ARP);
  arp_mask.hdr.ether_type = 0xffff;
  arp_pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  arp_pattern[0].spec = &arp_spec;
  arp_pattern[0].mask = &arp_mask;
  arp_pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

  drop_actions[0].type = RTE_FLOW_ACTION_TYPE_DROP;
  drop_actions[1].type = RTE_FLOW_ACTION_TYPE_END;
  memset(&ethernet_spec, 0, sizeof(ethernet_spec));
  memset(&ethernet_mask, 0, sizeof(ethernet_mask));
  drop_pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
  drop_pattern[0].spec = &ethernet_spec;
  drop_pattern[0].mask = &ethernet_mask;
  drop_pattern[1].type = RTE_FLOW_ITEM_TYPE_END;

  result = rte_flow_validate(
      port_id, &attributes, pattern, actions, &error);
  if (result != 0) {
    AXIO_ERROR("Flow steering validation failed: %s\n", error.message);
    goto done;
  }
  result = rte_flow_validate(
      port_id, &attributes, arp_pattern, arp_actions, &error);
  if (result != 0) {
    AXIO_ERROR("ARP flow validation failed: %s\n", error.message);
    goto done;
  }
  result = rte_flow_validate(
      port_id, &attributes, drop_pattern, drop_actions, &error);
  if (result != 0) {
    AXIO_ERROR("Drop-flow validation failed: %s\n", error.message);
    goto done;
  }

  this->flow_ = rte_flow_create(
      port_id, &attributes, pattern, actions, &error);
  if (this->flow_ == nullptr) {
    AXIO_ERROR("Failed to create steering flow: %s\n", error.message);
    goto done;
  }
  if (rte_flow_create(
          port_id, &attributes, arp_pattern, arp_actions, &error) == nullptr) {
    AXIO_ERROR("Failed to create ARP flow: %s\n", error.message);
    goto done;
  }
  if (rte_flow_create(
          port_id, &attributes, drop_pattern, drop_actions, &error) == nullptr) {
    AXIO_ERROR("Failed to create drop flow: %s\n", error.message);
    goto done;
  }

  AXIO_INFO(
      "Offloaded flow rules: workspace(%u), numa(%u), port(%u), "
      "UDP port(%u), queue(%lu)\n",
      workspace_id, numa_node, port_id,
      static_cast<uint32_t>(workspace_id) + kDefaultUdpPort,
      queue_pair_id);

done:
  return;
}

void DpdkDispatcher::_resolve_physical_port() {
  struct rte_ether_addr mac;
  rte_eth_macaddr_get(this->physical_port(), &mac);
  memcpy(&this->resolve_.mac_addr_.bytes_, &mac.addr_bytes,
         sizeof(this->resolve_.mac_addr_.bytes_));

  parse_ip_address(&this->resolve_.ipv4_addr_, this->local_ip());

  // Resolve RSS indirection table size
  struct rte_eth_dev_info dev_info;
  rte_eth_dev_info_get(this->physical_port(), &dev_info);

  const std::string drv_name = dev_info.driver_name;
  // rt_assert(drv_name == "net_mlx4" or drv_name == "net_mlx5" or
  //               drv_name == "mlx5_pci",
  //           "Axio supports only mlx4 or mlx5 devices with DPDK");

  // if (std::string(dev_info.driver_name) == "net_mlx4") {
  //   // MLX4 NICs report a reta size of zero, but they use 128 internally
  //   rt_assert(dev_info.reta_size == 0,
  //             "Unexpected RETA size for MLX4 NIC (expected zero)");
  //   this->resolve_.reta_size_ = 128;
  // } else {
  //   this->resolve_.reta_size_ = dev_info.reta_size;
  //   rt_assert(this->resolve_.reta_size_ >= kMaxQueuesPerPort,
  //             "Too few entries in NIC RSS indirection table");
  // }

  // Resolve bandwidth. XXX: For some reason, rte_eth_link_get() does not work
  // in secondary DPDK processes (up to DPDK 21.05).
  struct rte_eth_link link;
  if (this->process_type_ == DpdkProcType::kPrimary) {
    rte_eth_link_get(static_cast<uint8_t>(this->physical_port()), &link);
    rt_assert(link.link_status == RTE_ETH_LINK_UP,
              "Port " + std::to_string(this->physical_port()) + " is down.");
  } else {
    link = g_memzone->link(this->physical_port());
  }

  if (link.link_speed != RTE_ETH_SPEED_NUM_NONE) {
    // link_speed is in Mbps. The 10 Gbps check below is just a sanity check.
    rt_assert(link.link_speed >= 10000, "Link too slow");
    this->resolve_.bandwidth_ =
        static_cast<size_t>(link.link_speed) * 1000 * 1000 / 8.0;
  } else {
    AXIO_WARN(
        "Port %u bandwidth not reported by DPDK. Using default 10 Gbps.\n",
        this->physical_port());
    link.link_speed = 10000;
    this->resolve_.bandwidth_ = 10.0 * (1000 * 1000 * 1000) / 8.0;
  }

  char mac_str[64];
  format_ethernet_address(&this->resolve_.mac_addr_, mac_str);
  AXIO_INFO(
      "Resolved port %u: MAC %s, IPv4 %u.%u.%u.%u, RETA size %zu entries, bandwidth "
      "%.1f Gbps\n",
      this->physical_port(), mac_str,
      AXIO_IPV4_BYTES(this->resolve_.ipv4_addr_.ipv4_), this->resolve_.reta_size_,
      this->resolve_.bandwidth_ * 8.0 / (1000 * 1000 * 1000));
}

/// Allocate one DPDK packet buffer.
rte_mbuf* dpdk_allocate_buffer(void* mempool) {
  return rte_pktmbuf_alloc(static_cast<rte_mempool*>(mempool));
}

/// Allocate multiple DPDK packet buffers.
uint8_t dpdk_allocate_buffers(void* mempool, rte_mbuf** buffers,
                              size_t count) {
  return rte_pktmbuf_alloc_bulk(
      static_cast<rte_mempool*>(mempool), buffers, count);
}

void dpdk_deallocate_buffer(rte_mbuf* buffer, void* mempool) {
  AXIO_UNUSED(mempool);
  rte_pktmbuf_free(buffer);
}

void dpdk_deallocate_buffers(rte_mbuf** buffers, size_t count, void* mempool) {
  AXIO_UNUSED(mempool);
  rte_pktmbuf_free_bulk(buffers, count);
}

WorkspaceHeader* dpdk_extract_workspace_header(rte_mbuf* buffer) {
  return AXIO_MBUF_WORKSPACE_HEADER(buffer);
}

void dpdk_set_buffer_payload(rte_mbuf* buffer, char* udp_header,
                             char* workspace_header, size_t payload_size) {
  rte_pktmbuf_reset(buffer);
  AXIO_MBUF_APPEND_DATA(
      buffer, AXIO_MBUF_TOTAL_HEADER_LENGTH + payload_size);

  rte_memcpy(AXIO_MBUF_UDP_HEADER(buffer), udp_header, sizeof(udphdr));
  rte_memcpy(AXIO_MBUF_WORKSPACE_HEADER(buffer), workspace_header,
             sizeof(WorkspaceHeader));
  if (AXIO_UNLIKELY(payload_size == 0)) {
    return;
  }
  char* payload_ptr = AXIO_MBUF_WORKSPACE_PAYLOAD(buffer);
  memset(payload_ptr, 'a', payload_size - 1);
  payload_ptr[payload_size - 1] = '\0';
}

void dpdk_copy_buffer_payload(rte_mbuf* destination, rte_mbuf* source,
                              char* udp_header, char* workspace_header,
                              size_t payload_size) {
  rte_pktmbuf_reset(destination);
  AXIO_MBUF_APPEND_DATA(
      destination, AXIO_MBUF_TOTAL_HEADER_LENGTH + payload_size);

  char* payload_ptr = AXIO_MBUF_WORKSPACE_PAYLOAD(destination);
  rte_memcpy(
      AXIO_MBUF_UDP_HEADER(destination), udp_header, sizeof(udphdr));
  rte_memcpy(AXIO_MBUF_WORKSPACE_HEADER(destination), workspace_header,
             sizeof(WorkspaceHeader));
  rte_memcpy(
      payload_ptr, AXIO_MBUF_WORKSPACE_PAYLOAD(source), payload_size);
}

void DpdkDispatcher::_initialize_memory_region_functions() {
  this->memory_region_info_ = new MemoryRegionInfo<rte_mbuf>(
      this->mempool_, &dpdk_allocate_buffer, &dpdk_deallocate_buffer,
      &dpdk_allocate_buffers, &dpdk_deallocate_buffers,
      &dpdk_set_buffer_payload, &dpdk_extract_workspace_header,
      &dpdk_copy_buffer_payload);
}

}  // namespace axio
