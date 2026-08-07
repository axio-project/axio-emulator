/**
 * @file roce_dispatcher.h
 * @brief RoCE dispatcher interface for ConnectX NICs.
 */

#pragma once
#include "common.h"
#include "dispatcher.h"
#include "buffer.h"
#include "huge_alloc.h"
#include "verbs_common.h"

#include "util/logger.h"
#include "util/lock_free_queue.h"
#include "util/mgnt_connection.h"
#include "util/qpinfo.hh"
#include "util/rule_table.h"

#include <iomanip>

namespace axio {

class RoceDispatcher : public Dispatcher {
 public:
  static constexpr size_t kInvalidQueuePairId = SIZE_MAX;
  static constexpr size_t kMaxRoutingInfoSize = 48;
  static constexpr size_t kReceiveQueueDepth = kNumRxRingEntries;
  static constexpr size_t kSendQueueDepth = kNumTxRingEntries;
  static constexpr size_t kMbufSize = 4096;
  static constexpr size_t kMemoryRegionSize = kMemPoolSize * kMbufSize;
  static constexpr size_t kMaxInline = 60;
  // Ideally the connection handshake would negotiate this queue key.
  static constexpr uint32_t kQueueKey = 0x0205;
  static constexpr size_t kGlobalRouteHeaderBytes = 40;
  static constexpr size_t kMaxDataPerPacket = kMtu - sizeof(iphdr);

  /**
   * @brief Generic struct to store routing info for any transport.
   *
   * This can contain both cluster-wide valid members (e.g., {LID, QPN}), and
   * members that are only locally valid (e.g., a pointer to \p ibv_ah).
   */
  struct RoutingInfo {
    uint8_t buf_[kMaxRoutingInfoSize];
  };

  /**
   * @brief Session endpoint routing info for InfiniBand.
   *
   * The client fills in its \p port_lid_ and \p queue_pair_number_, which are
   * resolved into
   * the address handle by the server. Similarly for the server.
   *
   * \p port_lid_, \p queue_pair_number_, and \p gid_ have cluster-wide meaning,
   * but \p address_handle_ is local to this machine.
   *
   * The \p ibv_ah struct cannot be inlined into a RoutingInfo struct because
   * the device driver internally uses a larger struct (e.g., \p mlx4_ah for
   * ConnectX-3) which contains \p ibv_ah.
   */
  struct IbRoutingInfo {
    // Fields that are meaningful cluster-wide
    uint16_t port_lid_;
    uint32_t queue_pair_number_;
    union ibv_gid gid_;  // RoCE only

    // Fields that are meaningful only locally
    struct ibv_ah* address_handle_;
  };

  RoceDispatcher(uint8_t workspace_id, uint8_t physical_port, size_t numa_node,
                 UserConfig* user_config);
  ~RoceDispatcher();

  size_t collect_tx_packets();

  /**
   * @brief Flush the dispatcher tx queue to the NIC. Workspace will be blocked
   * until all packets are sent.
   */
  size_t flush_tx();

  /**
   * @brief Receive packets from the NIC and put them into the dispatcher rx queue.
   */
  ReceiveBurstResult receive_burst(bool capture_completion_timestamp);

  /**
   * @brief Dispatch packets from the dispatcher rx queue to the worker rx queue
   * based on packet UDP field. Workspace will be blocked until all packets are
   * dispatched.
   */
  size_t dispatch_rx_packets();

  /**
   * ----------------------User defined methods----------------------
   */
  /**
   * @brief Process packets inside the dispatcher before dispatching to the NIC.
   * @note TODO
   */
  template <PacketHandlerType handler>
  size_t handle_client_packets() {
    AXIO_UNUSED(handler);
    return 0;
  }

  /**
   * @brief Process packets before dispatching to the application thread.
   */
  template <PacketHandlerType handler>
  size_t handle_server_packets();

  /**
   * ----------------------Util methods----------------------
   */
  MemoryRegionInfo<Buffer>* memory_region() {
    return this->memory_region_info_;
  }
  size_t tx_queue_size() { return this->tx_queue_index_; }
  size_t rx_queue_size() { return this->pending_dispatch_count_; }

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

  size_t used_buffer_count() { return 0; }

  void set_tx_queue_index(size_t index) { this->tx_queue_index_ = index; }

 private:
  /** Resolved local RoCE port properties. */
  struct IbResolve : public VerbsResolve {
    IpAddress ipv4_addr_;
    uint16_t port_lid_ = 0;
    union ibv_gid gid_;
    uint8_t gid_index_ = 0;
    uint8_t mac_addr_[6] = {0};
  };

  size_t queue_pair_id_ = kInvalidQueuePairId;
  MemoryRegionInfo<Buffer>* memory_region_info_;

  HugeAlloc* huge_allocator_ = nullptr;
  ibv_mr* memory_region_;
  IbResolve resolved_port_;

  ibv_pd* protection_domain_ = nullptr;
  ibv_cq* send_completion_queue_ = nullptr;
  ibv_cq* receive_completion_queue_ = nullptr;
  ibv_qp* queue_pair_ = nullptr;

  ibv_ah* self_address_handle_ = nullptr;
  size_t remote_queue_pair_id_ = kInvalidQueuePairId;
  ibv_ah* remote_address_handle_ = nullptr;
  std::vector<ibv_ah*> address_handles_to_free_;
  IpAddress* destination_ip_ = nullptr;

  ibv_send_wr send_work_requests_[kSendQueueDepth];
  ibv_sge send_scatter_gather_[kSendQueueDepth];
  ibv_wc send_completions_[kSendQueueDepth];
  size_t send_head_index_ = 0;
  size_t send_tail_index_ = 0;
  size_t free_send_request_count_ = kSendQueueDepth;
  Buffer* send_ring_[kSendQueueDepth];
  Buffer* tx_queue_[kSendQueueDepth];
  size_t tx_queue_index_ = 0;

  ibv_recv_wr receive_work_requests_[kReceiveQueueDepth];
  ibv_sge receive_scatter_gather_[kReceiveQueueDepth];
  ibv_wc receive_completions_[kReceiveQueueDepth];
  size_t receive_head_index_ = 0;
  Buffer* receive_ring_[kReceiveQueueDepth];
  size_t receive_ring_head_ = 0;
  size_t pending_dispatch_count_ = 0;

  uint8_t workspace_queue_index_ = 0;
  std::vector<LockFreeQueue*> workspace_tx_queues_;
  LockFreeQueue* workspace_rx_queues_[kWorkspaceMaxNum] = {nullptr};

  RuleTable* rx_rule_table_ = new RuleTable();

#if AXIO_NODE_TYPE == AXIO_SERVER
  TcpServer* management_server_ = nullptr;
#elif AXIO_NODE_TYPE == AXIO_CLIENT
  TcpClient* management_client_ = nullptr;
#endif

  ibv_ah* _create_address_handle(const IbRoutingInfo* routing_info) const;
  void _fill_local_routing_info(RoutingInfo* routing_info) const;
  void _resolve_roce_port();
  void _initialize_verbs(uint8_t workspace_id);
  void _initialize_memory_region_functions(uint8_t numa_node);
  void _initialize_receives();
  void _initialize_sends();
  void _set_local_queue_pair_info(QueuePairInfo* queue_pair_info);
  bool _set_remote_queue_pair_info(QueuePairInfo* queue_pair_info);
  void _post_receives(size_t receive_count);
  uint8_t _resolve_packet_header(Buffer* buffer);
  size_t _transmit_burst(Buffer** buffers, size_t count);
};

}  // namespace axio
