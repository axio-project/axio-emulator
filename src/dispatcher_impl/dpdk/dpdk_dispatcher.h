/**
 * @file dpdk_datapath.h
 * @brief Transmit / Receive packets with a DPDK NIC
 */
#pragma once
#include "common.h"
#include "dispatcher.h"
#include "util/logger.h"
#include "util/numautils.h"
#include "util/lock_free_queue.h"
#include "util/rule_table.h"
#include "dispatcher_impl/ethhdr.h"
#include "dispatcher_impl/iphdr.h"
#include "dispatcher_impl/arphdr.h"
#include "ws_impl/ws_hdr.h"
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

namespace dperf {

class DpdkDispatcher : public Dispatcher {
  /**
   * ----------------------Parameters of DPDK----------------------
   */ 
  public:
    enum class DpdkProcType { kPrimary, kSecondary };
    static constexpr size_t kInvalidQpId = SIZE_MAX;
     // XXX: ixgbe does not support fast free offload, but i40e does
    static constexpr uint32_t kOffloads = RTE_ETH_TX_OFFLOAD_MULTI_SEGS;

    /// Number of mbufs in each mempool (one per Transport instance). The DPDK
    /// docs recommend power-of-two minus one mbufs per pool for best utilization.
    // static constexpr size_t kNumMbufs = (kNumRxRingEntries * 2 - 1);

    /// Per-element size for the packet buffer memory pool
    static constexpr size_t kMbufSize =
        (static_cast<uint32_t>(sizeof(struct rte_mbuf)) + RTE_PKTMBUF_HEADROOM + 2048); 

    static constexpr size_t kDpdkMempoolSize = kSizeMemPool - 1;

    /// Maximum data bytes (i.e., non-header) in a packet
    // static constexpr size_t kMaxDataPerPkt = (kMTU - sizeof(pkthdr_t));

  /**
   * ----------------------DPDK internal structures----------------------
   */ 
  public:
    /**
     * @brief Memzone created by the DPDK daemon process, shared by all DPDK processes.
     * Memzone maintains the queue pair status.
     */
    struct ownership_memzone_t {
      private:
        std::mutex mutex_;  /// Guard for reading/writing to the memzone
        size_t epoch_;      /// Incremented after each QP ownership change attempt
        size_t num_qps_available_;

        struct {
          /// pid_ is the PID of the process that owns QP #i. Zero means
          /// the corresponding QP is free.
          int pid_;

          /// proc_random_id_ is a random number installed by the process that owns
          /// QP #i. This is used to defend against PID reuse.
          size_t proc_random_id_;
        } owner_[kMaxPhyPorts][kMaxQueuesPerPort];

      public:
        struct rte_eth_link link_[kMaxPhyPorts];  /// Resolved link status

        void init() {
          new (&mutex_) std::mutex();  // Fancy in-place construction
          num_qps_available_ = kMaxQueuesPerPort;
          epoch_ = 0;
          memset(owner_, 0, sizeof(owner_));
        }

        size_t get_epoch() {
          const std::lock_guard<std::mutex> guard(mutex_);
          return epoch_;
        }

        size_t get_num_qps_available() {
          const std::lock_guard<std::mutex> guard(mutex_);
          return num_qps_available_;
        }

        std::string get_summary(size_t phy_port) {
          const std::lock_guard<std::mutex> guard(mutex_);
          std::ostringstream ret;
          ret << "[" << num_qps_available_ << " QPs of " << kMaxQueuesPerPort
              << " available] ";

          if (num_qps_available_ < kMaxQueuesPerPort) {
            ret << "[Ownership: ";
            for (size_t i = 0; i < kMaxQueuesPerPort; i++) {
              auto &owner = owner_[phy_port][i];
              if (owner.pid_ != 0) {
                ret << "[QP #" << i << ", "
                    << "PID " << owner.pid_ << "] ";
              }
            }
            ret << "]";
          }

          return ret.str();
        }

        /**
         * @brief Try to get a free QP
         *
         * @param phy_port The DPDK port ID to try getting a free QP from
         * @param proc_random_id A unique random process ID of the calling process
         *
         * @return If successful, the machine-wide global index of the free QP
         * reserved on phy_port. Else return kInvalidQpId.
         */
        size_t get_qp(size_t phy_port, size_t proc_random_id) {
          const std::lock_guard<std::mutex> guard(mutex_);
          epoch_++;
          const int my_pid = getpid();

          // Check for sanity
          for (size_t i = 0; i < kMaxQueuesPerPort; i++) {
            auto &owner = owner_[phy_port][i];
            if (owner.pid_ == my_pid && owner.proc_random_id_ != proc_random_id) {
              DPERF_ERROR(
                  "DPerf Dispatcher: Found another process with same PID (%d) as "
                  "mine. Process random IDs: mine %zu, other: %zu\n",
                  my_pid, proc_random_id, owner.proc_random_id_);
              return kInvalidQpId;
            }
          }

          for (size_t i = 0; i < kMaxQueuesPerPort; i++) {
            auto &owner = owner_[phy_port][i];
            if (owner.pid_ == 0) {
              owner.pid_ = my_pid;
              owner.proc_random_id_ = proc_random_id;
              num_qps_available_--;
              return i;
            }
          }
          return kInvalidQpId;
        }

        /**
         * @brief Try to return a QP that was previously reserved from this
         * ownership manager
         *
         * @param phy_port The DPDK port ID to try returning the QP to
         * @param qp_id The QP ID returned by this manager during reservation
         *
         * @return 0 if success, else errno
         */
        int free_qp(size_t phy_port, size_t qp_id) {
          const std::lock_guard<std::mutex> guard(mutex_);
          const int my_pid = getpid();
          epoch_++;
          auto &owner = owner_[phy_port][qp_id];
          if (owner.pid_ == 0) {
            DPERF_ERROR("DPerf Dispatcher: PID %d tried to already-free QP %zu.\n",
                      my_pid, qp_id);
            return EALREADY;
          }

          if (owner.pid_ != my_pid) {
            DPERF_ERROR(
                "DPerf Dispatcher: PID %d tried to free QP %zu owned by PID "
                "%d. Disallowed.\n",
                my_pid, qp_id, owner.pid_);
            return EPERM;
          }

          num_qps_available_++;
          owner_[phy_port][qp_id].pid_ = 0;
          return 0;
        }

        /// Free-up QPs reserved by processes that exited before freeing a QP.
        /// This is safe, but it can leak QPs because of PID reuse.
        void daemon_reclaim_qps_from_crashed(size_t phy_port) {
          const std::lock_guard<std::mutex> guard(mutex_);

          for (size_t i = 0; i < kMaxQueuesPerPort; i++) {
            auto &owner = owner_[phy_port][i];
            if (kill(owner.pid_, 0) != 0) {
              // This means that owner.pid_ is dead
              DPERF_WARN("DPerf Primary Dispatcher: Reclaiming QP %zu from crashed PID %d\n",
                        i, owner.pid_);
              num_qps_available_++;
              owner_[phy_port][i].pid_ = 0;
            }
          }
        }
    };

  /**
   * ----------------------DpdkDispatcher methods----------------------
   */ 
  public:
    /**
     * @brief Class Init
     * @param ws_id The workspace ID of the workspace that owns this dispatcher
     * @param phy_port The DPDK port ID to use for this dispatcher
     * @param numa_node The NUMA node to allocate memory from
     */
    DpdkDispatcher(uint8_t ws_id, uint8_t phy_port, size_t numa_node, UserConfig *user_config);
    ~DpdkDispatcher();

    /**
     * @brief Setup dpdk port and tx/rx rings
     */
    static void setup_phy_port(uint16_t phy_port, size_t numa_node,
                              DpdkProcType proc_type, uint8_t enabled_queue_num, size_t tx_batch, size_t rx_batch);

    /* ----------------------Defined in dpdk_dispatcher_dataplane.cc---------------------- */
    /**
     * @brief This method will iterate all workspaces in the workspace context 
     * and collect packets from their worker queues. 
    */
    size_t collect_tx_pkts();

    void fill_tx_pkts(size_t flow_size, size_t frame_size);

    void set_tx_queue_index(size_t index);

    void fill_rx_pkts(size_t flow_size);

    size_t get_rx_queue_index();
    
    void free_rx_queue();

    void set_rx_queue_index(size_t index);
    /**
     * @brief Flush the dispatcher tx queue to the NIC. Workspace will be blocked
     * until all packets are sent
    */
    size_t tx_flush();

    /**
     * @brief Construct an arp response, then send it out using rte_eth_tx_burst.
    */
    void tx_burst_for_arp(arp_hdr_t* arp_hdr);

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
     * @brief Check whether the received packet is a arp packet.
    */
    bool is_arp_packet(rte_mbuf *m);

    /**
     * @brief Parse the arp packet header, then send out an arp response if need.
    */
    void handle_arp_packet(rte_mbuf *m);

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
     *  \note     echo behavior:
     *            [1] swap the IP and MAC address;
     *            [2] insert packets to tx queue;
     *            [3] drop packets if the tx queue is full;
     *  \example  l2_reflector, e.g., OvS simple action
     */
    size_t echo_handler();

  /**
   * ----------------------Util methods----------------------
   */ 
  public:
    /// Get the mempool name to use for this port and queue pair ID
    static std::string get_mempool_name(size_t phy_port, size_t qp_id) {
      const std::string ret = std::string("dperf-mp-") + std::to_string(phy_port) +
                              std::string("-") + std::to_string(qp_id);
      rt_assert(ret.length() < RTE_MEMPOOL_NAMESIZE, "Mempool name too long");
      return ret;
    }

    static std::string dpdk_strerror() {
      return std::string(rte_strerror(rte_errno));
    }

    mem_reg_info<rte_mbuf> * get_mem_reg() {
      return mem_reg_info_;
    }

    rte_mempool * get_mempool() {
      return mempool_;
    }

    size_t get_tx_queue_size() {
      return tx_queue_idx_;
    }

    size_t get_rx_queue_size() {
      return rx_queue_idx_;
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
      return rte_mempool_in_use_count(mempool_);
    }

    size_t get_rx_used_desc() {
      return rte_eth_rx_queue_count(phy_port_, qp_id_);
    }

    // size_t get_tx_used_desc() {
    //   return rte_eth_tx_queue_count(phy_port_, qp_id_);
    // }

  /**
   * ----------------------Internal Parameters----------------------
   */ 
  private:
    DpdkDispatcher::DpdkProcType dpdk_proc_type_;
    size_t qp_id_ = kInvalidQpId;    ///< The RX/TX queue pair for this Transport
    // We don't use DPDK's lcore threads, so a shared mempool with per-lcore
    // cache won't work. Instead, we use per-thread pools with zero cached mbufs.
    rte_mempool *mempool_;
    mem_reg_info<rte_mbuf> *mem_reg_info_;
    /// Info resolved from \p phy_port, must be filled by constructor.
    struct {
      ipaddr_t ipv4_addr_;   // The port's IPv4 address in host-byte order
      eth_addr mac_addr_;    // The port's MAC address
      size_t bandwidth_;     // Link bandwidth in bytes per second
      size_t reta_size_;     // Number of entries in NIC RX indirection table
    } resolve_;
    eth_addr *dmac_ = nullptr;
    ipaddr_t *daddr_ = nullptr;
    
    /// tx / rx queue in dispatcher level
    struct rte_mbuf *tx_queue_[kNumTxRingEntries];
    struct rte_mbuf *rx_queue_[kNumRxRingEntries];
    size_t tx_queue_idx_ = 0, rx_queue_idx_ = 0;

    /// worker queues
    uint8_t ws_queue_idx_ = 0;
    std::vector<lock_free_queue*> ws_tx_queues_;
    lock_free_queue* ws_rx_queues_[kWorkspaceMaxNum] = {nullptr};  // Map ws_id to ws_queue

    /// Rule table for tx/rx packets to/from remote workspaces
    RuleTable *rx_rule_table_ = new RuleTable();

    /// flow rules to direct flow with corresponding udp dport to current dispatcher
    struct rte_flow *flow_ = nullptr;

  /**
   * ----------------------Internal Methods----------------------
   */
  private:
    /** 
     * @brief Direct the packets to corresponding cores 
     */
    void offload_flow_rules(uint8_t ws_id, uint8_t numa_id, uint8_t port_id, uint64_t qp_id);

    /** 
     * @brief Distory the flow rules to direct packets
     */
    void clear_flow_rules(uint8_t port_id);
    
    /**
     * @brief Resolve fields in \p resolve using \p phy_port
     * @throw runtime_error if the port cannot be resolved
     */
    void resolve_phy_port();

    /** 
     * @brief Initialize the memory registration and deregistration functions
     */
    void init_mem_reg_funcs();

    /// ----------------------dpdk dataplane methods----------------------
    /** 
     * @brief Poll for packets on this transport's RX queue until there are no more
     * packets left
     */
    void drain_rx_queue();

    /** 
     * @brief Generate a IP+UDP packet
     */
    void set_pkt_hdr(rte_mbuf *m);

    /** 
     * @brief Return workload type
     */
    uint8_t resolve_pkt_hdr(rte_mbuf *m);
};

}