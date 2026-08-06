/**
 * @file dpdk_dispatcher.h
 * @brief DPDK dispatcher interface.
 */
#pragma once
#include "common.h"
#include "dispatcher.h"
#include "util/logger.h"
#include "util/numautils.h"
#include "util/lock_free_queue.h"
#include "util/rule_table.h"
#include "util/qpinfo.hh"
#include "util/mgnt_connection.h"
#include "dispatcher_impl/ethhdr.h"
#include "dispatcher_impl/iphdr.h"
#include "dispatcher_impl/arphdr.h"
#include "ws_impl/workspace_header.h"
#include "mbuf_util.h"

#include <rte_common.h>
#include <rte_config.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_mbuf.h>
#include <rte_thash.h>
#include <rte_flow.h>
#include <rte_ethdev.h>
#include <rte_hash.h>

#include <signal.h>
#include <iomanip>
#include <set>
#include <stdexcept>
#include <netinet/udp.h>
#include <unordered_map>
#include <vector>

namespace axio {

class DpdkDispatcher : public Dispatcher {
 public:
  /**
   * ----------------------Parameters of DPDK----------------------
   */
  enum class DpdkProcType { kPrimary, kSecondary };
  static constexpr size_t kInvalidQpId = SIZE_MAX;
  // XXX: ixgbe does not support fast free offload, but i40e does.
  static constexpr uint32_t kOffloads = RTE_ETH_TX_OFFLOAD_MULTI_SEGS;

  /// Per-element size for the packet-buffer memory pool.
  static constexpr size_t kMbufSize =
      static_cast<uint32_t>(sizeof(struct rte_mbuf)) +
      RTE_PKTMBUF_HEADROOM + kMtu;

  static constexpr size_t kDpdkMempoolSize = kMemPoolSize - 1;

  /**
   * ----------------------DPDK internal structures----------------------
   */
  /**
   * @brief Queue-pair ownership state shared by DPDK processes.
   */
  class OwnershipMemzone {
   public:
    void init() {
      new (&this->mutex_) std::mutex();
      this->available_queue_pair_count_ = kMaxQueuesPerPort;
      this->epoch_ = 0;
      memset(this->owners_, 0, sizeof(this->owners_));
    }

    size_t epoch() {
      const std::lock_guard<std::mutex> guard(this->mutex_);
      return this->epoch_;
    }

    size_t available_queue_pair_count() {
      const std::lock_guard<std::mutex> guard(this->mutex_);
      return this->available_queue_pair_count_;
    }

    std::string summary(size_t physical_port) {
      const std::lock_guard<std::mutex> guard(this->mutex_);
      std::ostringstream result;
      result << "[" << this->available_queue_pair_count_ << " QPs of "
             << kMaxQueuesPerPort << " available] ";

      if (this->available_queue_pair_count_ < kMaxQueuesPerPort) {
        result << "[Ownership: ";
        for (size_t i = 0; i < kMaxQueuesPerPort; i++) {
          auto& owner = this->owners_[physical_port][i];
          if (owner.pid_ != 0) {
            result << "[QP #" << i << ", PID " << owner.pid_ << "] ";
          }
        }
        result << "]";
      }

      return result.str();
    }

    /// Acquire a free queue pair, or return kInvalidQpId when none is free.
    size_t acquire_queue_pair(size_t physical_port, size_t process_random_id) {
      const std::lock_guard<std::mutex> guard(this->mutex_);
      this->epoch_++;
      const int current_pid = getpid();

      for (size_t i = 0; i < kMaxQueuesPerPort; i++) {
        auto& owner = this->owners_[physical_port][i];
        if (owner.pid_ == current_pid &&
            owner.process_random_id_ != process_random_id) {
          AXIO_ERROR(
              "Axio Dispatcher: Found another process with PID %d. "
              "Process random IDs: mine %zu, other %zu\n",
              current_pid, process_random_id, owner.process_random_id_);
          return kInvalidQpId;
        }
      }

      for (size_t i = 0; i < kMaxQueuesPerPort; i++) {
        auto& owner = this->owners_[physical_port][i];
        if (owner.pid_ == 0) {
          owner.pid_ = current_pid;
          owner.process_random_id_ = process_random_id;
          this->available_queue_pair_count_--;
          return i;
        }
      }
      return kInvalidQpId;
    }

    /// Release a previously acquired queue pair. Returns zero or errno.
    int release_queue_pair(size_t physical_port, size_t queue_pair_id) {
      const std::lock_guard<std::mutex> guard(this->mutex_);
      const int current_pid = getpid();
      this->epoch_++;
      auto& owner = this->owners_[physical_port][queue_pair_id];
      if (owner.pid_ == 0) {
        AXIO_ERROR("Axio Dispatcher: PID %d tried to free QP %zu twice.\n",
                   current_pid, queue_pair_id);
        return EALREADY;
      }

      if (owner.pid_ != current_pid) {
        AXIO_ERROR(
            "Axio Dispatcher: PID %d tried to free QP %zu owned by PID %d. "
            "Disallowed.\n",
            current_pid, queue_pair_id, owner.pid_);
        return EPERM;
      }

      this->available_queue_pair_count_++;
      owner.pid_ = 0;
      return 0;
    }

    /// Reclaim QPs held by exited processes. PID reuse can prevent reclamation.
    void reclaim_crashed_queue_pairs(size_t physical_port) {
      const std::lock_guard<std::mutex> guard(this->mutex_);

      for (size_t i = 0; i < kMaxQueuesPerPort; i++) {
        auto& owner = this->owners_[physical_port][i];
        if (kill(owner.pid_, 0) != 0) {
          AXIO_WARN(
              "Axio Primary Dispatcher: Reclaiming QP %zu from crashed "
              "PID %d\n",
              i, owner.pid_);
          this->available_queue_pair_count_++;
          owner.pid_ = 0;
        }
      }
    }

    rte_eth_link& link(size_t physical_port) {
      return this->links_[physical_port];
    }

   private:
    struct Owner {
      int pid_;
      size_t process_random_id_;
    };

    std::mutex mutex_;
    size_t epoch_;
    size_t available_queue_pair_count_;
    Owner owners_[kMaxPhyPorts][kMaxQueuesPerPort];
    rte_eth_link links_[kMaxPhyPorts];
  };

  /**
   * @brief Construct a DPDK dispatcher.
   * @param ws_id Workspace ID that owns this dispatcher.
   * @param phy_port DPDK port ID used by this dispatcher.
   * @param numa_node NUMA node used for allocations.
   */
  DpdkDispatcher(uint8_t ws_id, uint8_t phy_port, size_t numa_node,
                 UserConfig* user_config);
  ~DpdkDispatcher();

  /// Configure a physical DPDK port for all enabled queue pairs.
  static void setup_physical_port(uint16_t physical_port, size_t numa_node,
                                  DpdkProcType process_type,
                                  uint8_t enabled_queue_count,
                                  size_t tx_batch_size,
                                  size_t rx_batch_size);

  /** Collect packets from workspace TX queues in round-robin order. */
  size_t collect_tx_packets();

  void fill_tx_packets(size_t flow_size, size_t frame_size);
  void set_tx_queue_index(size_t index);
  void fill_rx_packets(size_t flow_size);
  size_t rx_queue_index();
  void free_rx_queue();
  void set_rx_queue_index(size_t index);
  /// Flush the dispatcher TX queue. Blocks until every packet is sent.
  size_t flush_tx();

  /** Receive packets from the NIC into the dispatcher RX queue. */
  size_t receive_burst();

  /// Dispatch RX packets to workspace queues according to the UDP route.
  size_t dispatch_rx_packets();

  /**
   * ----------------------User defined methods----------------------
   */
  /// Process packets before dispatching them to the NIC.
  template <PacketHandlerType handler>
  size_t handle_client_packets() {
    AXIO_UNUSED(handler);
    return 0;
  }

  /// Process packets before dispatching them to an application workspace.
  template <PacketHandlerType handler>
  size_t handle_server_packets();

  /**
   * ----------------------Util methods----------------------
   */
  MemoryRegionInfo<rte_mbuf>* memory_region() {
    return this->memory_region_info_;
  }

  size_t tx_queue_size() { return this->tx_queue_index_; }
  size_t rx_queue_size() { return this->rx_queue_index_; }

  void add_workspace_tx_queue(LockFreeQueue* queue) {
    this->workspace_tx_queues_.push_back(queue);
  }

  uint8_t workspace_tx_queue_count() {
    return this->workspace_tx_queues_.size();
  }

  void add_workspace_rx_queue(uint8_t workspace_id, LockFreeQueue* queue) {
    this->workspace_rx_queues_[workspace_id] = queue;
  }

  void add_rx_route(uint8_t workload_type, uint8_t workspace_id) {
    this->rx_rule_table_->add_route(workload_type, workspace_id);
  }

  size_t used_buffer_count() {
    return rte_mempool_in_use_count(this->mempool_);
  }

  size_t rx_used_descriptor_count() {
    return rte_eth_rx_queue_count(this->physical_port(), this->queue_pair_id_);
  }

 private:
  DpdkProcType process_type_ = DpdkProcType::kPrimary;
  size_t queue_pair_id_ = kInvalidQpId;
  // Per-thread pools use no DPDK lcore cache because Axio threads are not
  // DPDK lcore threads.
  rte_mempool* mempool_ = nullptr;
  MemoryRegionInfo<rte_mbuf>* memory_region_info_ = nullptr;
  struct {
    IpAddress ipv4_addr_;
    EthernetAddress mac_addr_;
    size_t bandwidth_;
    size_t reta_size_;
  } resolve_;
  EthernetAddress* destination_mac_ = nullptr;
  IpAddress* destination_ip_ = nullptr;
  const char* remote_management_ip_ = "192.168.40.171";

  rte_mbuf* tx_queue_[kNumTxRingEntries] = {nullptr};
  rte_mbuf* rx_queue_[kNumRxRingEntries] = {nullptr};
  size_t tx_queue_index_ = 0;
  size_t rx_queue_index_ = 0;

  uint8_t workspace_queue_index_ = 0;
  std::vector<LockFreeQueue*> workspace_tx_queues_;
  LockFreeQueue* workspace_rx_queues_[kWorkspaceMaxNum] = {nullptr};

  RuleTable* rx_rule_table_ = new RuleTable();
  rte_flow* flow_ = nullptr;

  void _offload_flow_rules(uint8_t workspace_id, uint8_t numa_node,
                           uint8_t port_id, uint64_t queue_pair_id);
  void _clear_flow_rules(uint8_t port_id);
  void _resolve_physical_port();
  void _initialize_memory_region_functions();
  void _drain_rx_queue();
  void _set_packet_headers(rte_mbuf* buffer);
  uint8_t _resolve_packet_header(rte_mbuf* buffer);

  void _send_arp_reply(ArpHeader* arp_header);
  bool _is_arp_packet(rte_mbuf* buffer);
  void _handle_arp_packet(rte_mbuf* buffer);
  size_t _handle_echo();

  static std::string _mempool_name(size_t physical_port,
                                   size_t queue_pair_id) {
    // This prefix is part of the DPDK primary/secondary IPC contract.
    const std::string result =
        std::string("dperf-mp-") + std::to_string(physical_port) + "-" +
        std::to_string(queue_pair_id);
    rt_assert(result.length() < RTE_MEMPOOL_NAMESIZE,
              "Mempool name too long");
    return result;
  }

  static std::string _error_string() {
    return std::string(rte_strerror(rte_errno));
  }

  rte_mempool* _mempool() { return this->mempool_; }
};

}  // namespace axio
