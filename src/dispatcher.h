/**
 * @file dispatcher.h
 * @brief Common definitions for dispatcher backends.
 */
#pragma once
#include "axio/receive_burst_result.h"
#include "common.h"
#include "util/math_utils.h"
#include "dispatcher_impl/ethhdr.h"
#include "dispatcher_impl/iphdr.h"
#include "ws_impl/workspace_header.h"
#include "config.h"

#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include <functional>
#include <unordered_map>

namespace axio {
/// Available transport backend implementations.
enum class DispatcherType { kDpdk, kRoce };

/// Common state and compile-time limits shared by dispatcher backends.
class Dispatcher {
 public:
  /**
   * ----------------------Dispatcher-level parameters----------------------
   */
  static constexpr size_t kNumRxRingEntries = AXIO_CONFIG_RX_RING_ENTRIES;
  static_assert(is_power_of_two<size_t>(kNumRxRingEntries),
                "The number of RX ring entries must be a power of two.");
  static constexpr size_t kNumTxRingEntries = AXIO_CONFIG_TX_RING_ENTRIES;
  static_assert(is_power_of_two<size_t>(kNumTxRingEntries),
                "The number of TX ring entries must be a power of two.");
  static constexpr size_t kMemPoolSize = AXIO_CONFIG_MEMPOOL_SIZE;
  static constexpr size_t kMtu = AXIO_CONFIG_MTU;
  static_assert(is_power_of_two<size_t>(kMtu),
                "The MTU must be a power of two.");
  static constexpr size_t kMaxPayloadSize =
      kMtu - sizeof(iphdr) - sizeof(udphdr);
  static constexpr uint16_t kDefaultUdpPort = 10010;
  static constexpr uint16_t kDefaultMngtPort = 20086;

  /// Passive function table used by Workspace to access backend buffers.
  template <typename T>
  struct MemoryRegionInfo {
    using Allocate = T* (*)(void*);
    using AllocateBulk = uint8_t (*)(void*, T**, size_t);
    using Deallocate = void (*)(T*, void*);
    using DeallocateBulk = void (*)(T**, size_t, void*);
    using SetPayload = void (*)(T*, char*, char*, size_t);
    using ExtractWorkspaceHeader = WorkspaceHeader* (*)(T*);
    using CopyPayload = void (*)(T*, T*, char*, char*, size_t);

    void* dispatcher_memory_region_;
    Allocate allocate_;
    Deallocate deallocate_;
    AllocateBulk allocate_bulk_;
    DeallocateBulk deallocate_bulk_;
    SetPayload set_payload_;
    ExtractWorkspaceHeader extract_workspace_header_;
    CopyPayload copy_payload_;

    MemoryRegionInfo(void* memory_region, Allocate allocate,
                     Deallocate deallocate, AllocateBulk allocate_bulk,
                     DeallocateBulk deallocate_bulk, SetPayload set_payload,
                     ExtractWorkspaceHeader extract_workspace_header,
                     CopyPayload copy_payload)
        : dispatcher_memory_region_(memory_region),
          allocate_(allocate),
          deallocate_(deallocate),
          allocate_bulk_(allocate_bulk),
          deallocate_bulk_(deallocate_bulk),
          set_payload_(set_payload),
          extract_workspace_header_(extract_workspace_header),
          copy_payload_(copy_payload) {}
  };


  /**
   * ----------------------Dispatcher methods----------------------
   */ 
  /**
     * @brief Construct common dispatcher state.
     *
     * @param phy_port An Workspace object uses one port on a "datapath" NIC, which
     * refers to a NIC that supports DPDK. phy_port is the zero-based index of 
     * that port among active ports, same as the one passed to
     * `rte_eth_dev_info_get` for the DPDK transport. Multiple Workspace objects may
     * use the same phy_port.
     *
     * @throw runtime_error if construction fails
     */
  Dispatcher(DispatcherType dispatcher_type, uint8_t ws_id, uint8_t phy_port,
             size_t numa_node, UserConfig* user_config);
  ~Dispatcher();

  /**
   * ----------------------Util methods----------------------
   */ 
  static std::string name(DispatcherType dispatcher_type) {
    switch (dispatcher_type) {
      case DispatcherType::kDpdk:
        return "[DPDK]";
      case DispatcherType::kRoce:
        return "[RoCE]";
    }
    throw std::runtime_error("Axio: invalid dispatcher backend");
  }

  DispatcherType type() const { return this->dispatcher_type_; }
  uint16_t tx_batch_size() const { return this->tx_batch_size_; }
  uint16_t rx_batch_size() const { return this->rx_batch_size_; }
  uint16_t nic_tx_post_size() const { return this->nic_tx_post_size_; }
  uint16_t nic_rx_post_size() const { return this->nic_rx_post_size_; }

 protected:
  uint8_t physical_port() const { return this->physical_port_; }
  size_t numa_node() const { return this->numa_node_; }
  const char* local_ip() const { return this->local_ip_; }
  const char* remote_ip() const { return this->remote_ip_; }
  const EthernetAddress& local_mac() const { return this->local_mac_; }
  const EthernetAddress& remote_mac() const { return this->remote_mac_; }

 private:
  uint16_t tx_batch_size_ = 0;
  uint16_t rx_batch_size_ = 0;
  uint16_t nic_tx_post_size_ = 0;
  uint16_t nic_rx_post_size_ = 0;

  const DispatcherType dispatcher_type_;
  const uint8_t physical_port_;
  const size_t numa_node_;
  const char* local_ip_ = nullptr;
  const char* remote_ip_ = nullptr;
  EthernetAddress local_mac_{};
  EthernetAddress remote_mac_{};
};
}  // namespace axio

/**
 * ----------------------Include sub-class of Dispatcher----------------------
 */ 
#if AXIO_ROCE_MODE
  #include "dispatcher_impl/roce/roce_dispatcher.h"
#elif AXIO_DPDK_MODE
  #include "dispatcher_impl/dpdk/dpdk_dispatcher.h"
#endif
