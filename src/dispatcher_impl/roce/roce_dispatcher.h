/**
 * @file roce_datapath.h
 * @brief Transmit / Receive packets with a RoCE NIC (CX5 / CX7)
 */

#pragma once
#include "common.h"
#include "dispatcher.h"
#include "verbs_common.h"
#include "qpinfo.hh"
#include "huge_alloc.h"
#include "buffer.h"

#include "util/lock_free_queue.h"
#include "util/rule_table.h"
#include "util/logger.h"
#include "util/mgnt_connection.h"


#include <iomanip>

namespace dperf {

class RoceDispatcher : public Dispatcher {
  /**
   * ----------------------Parameters of RoCE----------------------
   */ 
  public:
    static constexpr size_t kInvalidQpId = SIZE_MAX;
    static constexpr size_t kMaxRoutingInfoSize = 48;  ///< Space for routing info

    static constexpr size_t kRQDepth = kNumRxRingEntries;   ///< RECV queue depth
    static constexpr size_t kSQDepth = kNumTxRingEntries;   ///< Send queue depth
    static constexpr size_t kMbufSize = 4096;    ///< RECV size (if UD, make sure GRH is included in first 64B, where kMbufSize = kMTU + GRH)
    static constexpr size_t kMemRegionSize = (kMemPoolSize) * kMbufSize;  ///< Memory region size

    static constexpr size_t kMaxInline = 60;   ///< Maximum send wr inline data

    /// Ideally, the connection handshake should establish a secure queue key.
    /// For now, anything outside 0xffff0000..0xffffffff (reserved by CX3) works.
    static constexpr uint32_t kQKey = 0x0205; 

    // static_assert(kSQDepth >= 2 * kTxBatchSize, "");  // Queue capacity check

    // Derived constants
    static constexpr size_t kGRHBytes = 40;
    
    /// Maximum data bytes (i.e., non-header) in a packet
    static constexpr size_t kMaxDataPerPkt = (kMTU - sizeof(iphdr));

  /**
   * ----------------------RoCE internal structures----------------------
   */ 

  /**
   * @brief Generic struct to store routing info for any transport.
   *
   * This can contain both cluster-wide valid members (e.g., {LID, QPN}), and
   * members that are only locally valid (e.g., a pointer to \p ibv_ah).
   */
  struct routing_info_t {
    uint8_t buf_[kMaxRoutingInfoSize];
  };

  /**
   * @brief Session endpoint routing info for InfiniBand.
   *
   * The client fills in its \p port_lid and \p qpn, which are resolved into
   * the address handle by the server. Similarly for the server.
   *
   * \p port_lid, \p qpn, and \p gid have cluster-wide meaning, but \p ah is
   * local to this machine.
   *
   * The \p ibv_ah struct cannot be inlined into a RoutingInfo struct because
   * the device driver internally uses a larger struct (e.g., \p mlx4_ah for
   * ConnectX-3) which contains \p ibv_ah.
   */
  struct ib_routing_info_t {
    // Fields that are meaningful cluster-wide
    uint16_t port_lid;
    uint32_t qpn;
    union ibv_gid gid;  // RoCE only

    // Fields that are meaningful only locally
    struct ibv_ah *ah;
  };

  /**
   * ----------------------RoceDispatcher methods----------------------
   */ 
  public:
    /**
     * @brief Class setup
     * @param ws_id The workspace ID of the workspace that owns this dispatcher
     * @param phy_port The RDMA NIC port ID to use for this dispatcher
     * @param numa_node The NUMA node to allocate memory from
     */
    RoceDispatcher(uint8_t ws_id, uint8_t phy_port, size_t numa_node, UserConfig *user_config);
    ~RoceDispatcher();

    /* ----------------------Defined in roce_dispatcher_dataplane.cc---------------------- */
    /**
     * @brief This method will iterate all workspaces in the workspace context 
     * and collect packets from their worker queues. 
    */
    size_t collect_tx_pkts();

    /**
     * @brief Flush the dispatcher tx queue to the NIC. Workspace will be blocked
     * until all packets are sent
    */
    size_t tx_flush();

    /**
     * @brief Receive packets from the NIC and put them into the dispatcher rx queue.
    */
    size_t rx_burst();

    /**
     * @brief Dispatch packets from the dispatcher rx queue to the worker rx queue 
     * based on packet UDP field. Workspace will be blocked until all packets are
     * dispatched.
    */
    size_t dispatch_rx_pkts();

  /**
   * ----------------------User defined methods----------------------
   */ 
  public:
    /**
     *  @brief  Processing packets inside dispatcher before dispatching packets to
     *          NIC
     *  @note   TODO
     */
    template<pkt_handler_type_t handler>
    size_t pkt_handler_client() {return 0;}

    /**
     *  @brief  Processing packets inside dispatcher before dispatching packets to
     *          application thread
     */
    template<pkt_handler_type_t handler>
    size_t pkt_handler_server();

  /**
   * ----------------------Util methods----------------------
   */ 
  public:
    mem_reg_info<Buffer> * get_mem_reg() {
      return mem_reg_info_;
    }
    size_t get_tx_queue_size() {
      return tx_queue_idx_;
    }

    size_t get_rx_queue_size() {
      return wait_for_disp_;
    }

    void add_ws_tx_queue(lock_free_queue *queue) {
      ws_tx_queues_.push_back(queue);
    }

    uint8_t get_ws_tx_queue_size() {
      return ws_tx_queues_.size();
    }

    void add_ws_rx_queue(uint8_t ws_id, lock_free_queue *queue) {
      ws_rx_queues_[ws_id] = queue;
    }

    void add_rx_rule(uint8_t workload_type, uint8_t ws_id) {
      rx_rule_table_->add_route(workload_type, ws_id);
    }

    size_t get_used_mbuf_num() {
      /// TODO
      return 0;
    }

    size_t get_rx_used_desc() {
      return wait_for_disp_ + ring_head_ - recv_head_;
    }

    void set_tx_queue_index(size_t index) {
      tx_queue_idx_ = index;
    }
  
  private:
    /// Create an address handle using this routing info
    struct ibv_ah *create_ah(const ib_routing_info_t *) const;
    void fill_local_routing_info(routing_info_t *routing_info) const;

  /**
   * ----------------------Internal Parameters----------------------
   */ 
  private:
    size_t qp_id_ = kInvalidQpId;    ///< The RX/TX queue pair for this Transport
    mem_reg_info<Buffer> *mem_reg_info_;

    /// The hugepage allocator for this dispatcher
    HugeAlloc *huge_alloc_ = nullptr;    /// Huge page allocator for RDMA buffers
    ibv_mr *mr_;

    /// Info resolved from \p phy_port, must be filled by constructor.
    class IBResolve : public VerbsResolve {
    public:
      ipaddr_t ipv4_addr_;        ///< The port's IPv4 address in host-byte order
      uint16_t port_lid = 0;      ///< Port LID. 0 is invalid.
      union ibv_gid gid;          ///< GID, used only for RoCE
      uint8_t gid_index = 0;      ///< GID index, used only for RoCE
      uint8_t mac_addr[6] = {0};  ///< MAC address of the device port
    } resolve_;

    /// parameters for qp init
    struct ibv_pd *pd_ = nullptr;   /// protection domain
    struct ibv_cq *send_cq_ = nullptr, *recv_cq_ = nullptr;
    struct ibv_qp *qp_ = nullptr;

    /// An address handle for this endpoint's port. Used for tx_flush().
    struct ibv_ah *self_ah_ = nullptr;
    size_t remote_qp_id_ = kInvalidQpId;  ///< The remote QP ID
    struct ibv_ah *remote_ah_ = nullptr;  ///< An address handle for the remote endpoint's port.
    /// Address handles that we must free in the destructor
    std::vector<ibv_ah *> ah_to_free_vec;
    ipaddr_t *daddr_ = nullptr;  ///< Destination IP address

    /// parameters for tx/rx ring
    // SEND
    struct ibv_send_wr send_wr[kSQDepth];  /// +1 for unconditional ->next
    struct ibv_sge send_sgl[kSQDepth];
    struct ibv_wc send_wc[kSQDepth];
    size_t send_head_ = 0;      ///< Index of current posted SEND buffer
    size_t send_tail_ = 0;      ///< Index of current un-posted SEND buffer
    size_t free_send_wr_num_ = kSQDepth;  ///< Number of free send wr
    Buffer *sw_ring_[kSQDepth];  ///< TX ring entries
    Buffer *tx_queue_[kSQDepth];
    size_t tx_queue_idx_ = 0;
    // RECV
    struct ibv_recv_wr recv_wr[kRQDepth];
    struct ibv_sge recv_sgl[kRQDepth];
    struct ibv_wc recv_wc[kRQDepth];
    size_t recv_head_ = 0;      ///< Index of current un-posted RECV buffer

    Buffer *rx_ring_[kRQDepth];  ///< RX ring entries
    size_t ring_head_ = 0;      ///< Index of rx ring
    size_t wait_for_disp_ = 0;  ///< Number of RECVs to batch before dispatching

    /// worker queues
    uint8_t ws_queue_idx_ = 0;
    std::vector<lock_free_queue*> ws_tx_queues_;
    lock_free_queue* ws_rx_queues_[kWorkspaceMaxNum] = {nullptr};  // Map ws_id to ws_queue

    /// Rule table for tx/rx packets to/from remote workspaces
    RuleTable *rx_rule_table_ = new RuleTable();

    /// flow rules to direct flow with corresponding udp dport to current dispatcher
    // struct rte_flow *flow_ = nullptr;

    /// Mgnt TCP connection
  #if NODE_TYPE == SERVER
    TCPServer *mgnt_server = nullptr;
  #elif NODE_TYPE == CLIENT
    TCPClient *mgnt_client = nullptr;
  #endif

  /**
   * ----------------------Internal Methods----------------------
   */
  private:
    /**
     * @brief Resolve InfiniBand-specific fields in \p resolve
     * @throw runtime_error if the port cannot be resolved
     */
    void roce_resolve_phy_port();

    /**
     * @brief Initialize structures: device
     * context, protection domain, and queue pair.
     *
     * @throw runtime_error if initialization fails
     */
    void init_verbs_structs(uint8_t ws_id);

    /// Initialize the memory registration and deregistration functions
    void init_mem_reg_funcs(uint8_t numa_node);

    /// Initialize constant fields of RECV descriptors, fill in the Rpc's
    ///  RX ring, and fill the RECV queue.
    void init_recvs();

    void init_sends();  ///< Initialize constant fields of SEND work requests

    void set_local_qp_info(QPInfo *qp_info);  ///< Set local QP info
    bool set_remote_qp_info(QPInfo *qp_info);  ///< Set remote QP info

    // roce_dispatcher_dataplane.cc
    void post_recvs(size_t num_recvs);
    uint8_t resolve_pkt_hdr(Buffer *m);
    size_t tx_burst(Buffer **tx, size_t nb_tx);
};

}