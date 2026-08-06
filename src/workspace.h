/**
 * @file workspace.h
 * @brief A workspace is an executor for datapath pipeline, 
 * it will be launched by thread/datapath OS/DOCA
 */

#pragma once
#include "common.h"
#include "config.h"
#include "dispatcher.h"
#include "util/logger.h"
#include "util/lock_free_queue.h"
#include "util/rule_table.h"
#include "util/net_stats.h"
#include "util/timer.h"
#include "util/numautils.h"
#include "util/rand.h"
#include "util/kv.h"

#include "ws_impl/workspace_context.h"
#include "ws_impl/ws_hdr.h"

#include <mutex>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace axio {
/**
 * ----------------------General definations----------------------
 */ 
#define DISPATCHER 1
#define WORKER 2
#define NIC_OFFLOAD 4
#define DISPATCHER_AND_WORKER 3

using phase_t = void (Workspace<AXIO_DISPATCHER_TYPE>::*)();

template <class TDispatcher>
class Workspace {
 private:
  /**
   * ----------------------Parameters tuned by Axio----------------------
   */ 
  uint16_t app_tx_message_batch_size_ = 0;
  uint16_t app_rx_message_batch_size_ = 0;

  /**
   * ----------------------Parameters in Application level----------------------
   */ 
  /// TX specific
  static constexpr size_t kAppRequestPktsNum = ceil((double)kAppReqPayloadSize / (double)Dispatcher::kMaxPayloadSize);  // number of packets in a request message
  static constexpr size_t kAppFullPaddingSize = Dispatcher::kMaxPayloadSize - sizeof(ws_hdr);
  static constexpr size_t kAppLastPaddingSize = kAppReqPayloadSize - (kAppRequestPktsNum - 1) * Dispatcher::kMaxPayloadSize - sizeof(ws_hdr);
  // RX specific
  static constexpr size_t kAppResponsePktsNum = ceil((double)kAppRespPayloadSize / (double)Dispatcher::kMaxPayloadSize); // number of packets in a response message
  static constexpr size_t kAppRespFullPaddingSize = Dispatcher::kMaxPayloadSize - sizeof(ws_hdr);
  
  /**
   * ----------------------Workspace internal structures----------------------
   */ 
  
  /**
   * ----------------------Workerspace methods----------------------
   */ 
 public:
    /**
     * @brief Construct the Workspace object.
     *
     * @param context pointer to the workspace context
     * @param ws_id The workspace id, which is unique among all workspaces
     * @param ws_type The workspace type, can be dispatcher, worker, or both
     * @param numa_node The numa node that the workspace is located
     * @param phy_port An Workspace object uses one port on a "datapath" NIC, which
     * refers to a NIC that supports DPDK. phy_port is the zero-based index of 
     * that port among active ports, same as the one passed to
     * `rte_eth_dev_info_get` for the DPDK dispatcher. Multiple Workspace objects may 
     * use the same phy_port.
     *
     * @throw runtime_error if construction fails
     */
    Workspace(WsContext *context, uint8_t ws_id, uint8_t ws_type, uint8_t numa_node, uint8_t phy_port, 
              std::vector<axio::phase_t> *ws_loop, UserConfig *user_config);
    /// Destroy the Workspace from a foreground thread
    ~Workspace();

    /**
     * @brief Launch the workspace to execute datapath pipeline
    */
    void launch();

    /**
     * @brief Run the pipeline loops for a given number of iterations
     * @param iteration The number of iterations to run
     * @param seconds The number of seconds for each iteration
    */
    void run_event_loop_timeout_st(uint8_t iteration, uint8_t seconds);

    /* ----------------------Functions used in pipeline execution---------------------- */
    /**
     * @brief App tx phase, step 1: apply mbufs. Stall occurs when there is no available mbuf
     */
    void apply_mbufs() {
    #if AXIO_ENABLE_INFLIGHT_LIMIT
      // Block until the workload has enough inflight-message budget.
      if (!this->tx_rule_table_->try_acquire_inflight_budget(
              this->workload_type_, this->app_tx_message_batch_size_)) {
        this->inflight_budget_acquired_ = false;
        return;
      }
      this->inflight_budget_acquired_ = true;
    #endif

      size_t s_tick = rdtsc();
      while (AXIO_UNLIKELY(this->_allocate_bulk(this->tx_mbuf_, kAppRequestPktsNum * this->app_tx_message_batch_size_) != 0)) {
        net_stats_app_apply_mbuf_stalls();
      }

      net_stats_app_tx_stall_duration(s_tick);

      // Measure mempool usage in AXIO_ONE_STAGE mode to diagnose whether stalls are
      // caused by allocation conflicts or mempool congestion. The dispatcher
      // thread must have an application workload for this measurement.
    #ifdef AXIO_ONE_STAGE
      if (this->ws_type_ & DISPATCHER) {
        uint32_t usage = this->dispatcher_->used_buffer_count();
        net_stats_mbuf_usage(usage);
      }
    #endif
    }

    /**
     * @brief App tx phase, step 2: generate packets. Drop occurs when the tx queue is full
    */
    void generate_pkts() {
      #if AXIO_ENABLE_INFLIGHT_LIMIT
        if (!this->inflight_budget_acquired_) return;
      #endif
      size_t s_tick = rdtsc();
      /// partially set udp header
      udphdr uh;
      uh.source = this->ws_id_;
      uh.dest = this->tx_rule_table_->select_next(this->workload_type_);
      /// set workspace header
      ws_hdr hdr;
      hdr.workload_type_ = this->workload_type_;
      hdr.segment_num_ = kAppRequestPktsNum;
      AXIO_MEMORY_BUFFER_TYPE **mbuf_ptr = this->tx_mbuf_;
      /// Insert payload to mbufs
      for (size_t msg_idx = 0; msg_idx < this->app_tx_message_batch_size_; msg_idx++) {
        /// TBD: Perform extra memory access and calculation for each message
        /// Iterate all messages in a batch
        for (size_t seg_idx = 0; seg_idx < kAppRequestPktsNum - 1; seg_idx++) {
          /// Iterate all segments in a message
          this->_write_payload(*mbuf_ptr, (char*)&uh, (char*)&hdr, kAppFullPaddingSize);
          mbuf_ptr++;
        }
        this->_write_payload(*mbuf_ptr, (char*)&uh, (char*)&hdr, kAppLastPaddingSize);
        mbuf_ptr++;
      }
      /// Insert packets to worker tx queue
      size_t drop_num = 0;
      for (size_t i = 0; i < kAppRequestPktsNum * this->app_tx_message_batch_size_; i++) {
        if (AXIO_UNLIKELY(!this->tx_queue_->enqueue((uint8_t*)this->tx_mbuf_[i]))) {
          /// Drop the packet if the tx queue is full
          this->_deallocate(this->tx_mbuf_[i]);
          drop_num++;
        }
      }
      net_stats_app_tx(this->app_tx_message_batch_size_ * kAppRequestPktsNum - drop_num);
      net_stats_app_drops(drop_num);
      net_stats_app_tx_duration(s_tick);
      #ifdef AXIO_ONE_STAGE
        this->tx_queue_->reset_tail();
        s_tick = rdtsc();
        this->_deallocate_bulk(this->tx_mbuf_, kAppRequestPktsNum * this->app_tx_message_batch_size_);
        net_stats_app_tx_stall_duration(s_tick);
        // for (size_t i = 0; i < kAppRequestPktsNum * this->app_tx_message_batch_size_; i++) {
        //   this->_deallocate(this->tx_mbuf_[i]);
        // }
      #endif
    }

    /**
     * @brief App rx phase: handle received messages. 
    */
    void app_handler() {
    #ifdef AXIO_ONE_STAGE
        this->_fill_queue(this->rx_queue_, AXIO_FLOW_SIZE);
    #endif
      size_t s_tick = rdtsc();
      size_t rx_size = this->rx_queue_->size();

      /**
       *  @brief  Mock the processing of one message.
       *  @param  msg       pointer to the message to be processed
       *  @param  ticks     specified processing ticks
      */
      auto mock_process_message = [&](AXIO_MEMORY_BUFFER_TYPE** msg, uint64_t ticks, size_t msg_num) {
        uint64_t s_tick, passed_ticks = 0;

        s_tick = rdtsc();
        // step 1: exec message processing handler
        #if AXIO_NODE_TYPE == AXIO_CLIENT
          this->_handle_client_messages(msg, msg_num);
        #else
          this->template _handle_server_messages<AXIO_RX_MESSAGE_HANDLER>(msg, msg_num);
        #endif
  
        // step 2: mock remain ticks
        do {
          passed_ticks = rdtsc() - s_tick;
        } while(passed_ticks < ticks);
      };

      /// enter rule, receive >= this->app_rx_message_batch_size_ requests to process
    #if AXIO_NODE_TYPE == AXIO_CLIENT
      size_t msg_num = rx_size / kAppResponsePktsNum;
      if (msg_num < this->app_rx_message_batch_size_)
        return;
      /// handle message
      for (size_t i = 0; i < msg_num; i++) {
        for (size_t j = 0; j < kAppResponsePktsNum; j++) {
          this->rx_mbuf_buffer_[i*kAppResponsePktsNum + j] = (AXIO_MEMORY_BUFFER_TYPE*)this->rx_queue_->dequeue();
          rt_assert(this->rx_mbuf_buffer_[i*kAppResponsePktsNum + j] != nullptr, "Get invalid mbuf!");
        }
      }
      mock_process_message(this->rx_mbuf_buffer_, kAppTicksPerMsg * msg_num, msg_num);
      net_stats_app_rx(msg_num * kAppResponsePktsNum);
    #else
      size_t msg_num = rx_size / kAppRequestPktsNum;
      if (msg_num < this->app_rx_message_batch_size_)
        return;
      /// handle message
      for (size_t i = 0; i < msg_num; i++) {
        for (size_t j = 0; j < kAppRequestPktsNum; j++) {
          this->rx_mbuf_buffer_[i*kAppRequestPktsNum + j] = (AXIO_MEMORY_BUFFER_TYPE*)this->rx_queue_->dequeue();
          rt_assert(this->rx_mbuf_buffer_[i*kAppRequestPktsNum + j] != nullptr, "Get invalid mbuf!");
        }
      }
      mock_process_message(this->rx_mbuf_buffer_, kAppTicksPerMsg * msg_num, msg_num);
      net_stats_app_rx(msg_num * kAppRequestPktsNum);
    #endif
      net_stats_app_rx_duration(s_tick);

      #ifdef AXIO_ONE_STAGE
        size_t size = this->tx_queue_->size();
        for (size_t j = 0; j < size; j++) {
          // this->_deallocate((AXIO_MEMORY_BUFFER_TYPE *)this->tx_queue_->dequeue());
          this->rx_queue_->enqueue(this->tx_queue_->dequeue());
        }
      #endif
    }

    /**
     * @brief Dispatcher tx phase: collect packets from worker tx queue and flush to the nic. 
     * Stall occurs when the tx ring is full.
    */
    void bursted_tx() {
      #ifdef AXIO_ONE_STAGE
        this->_fill_queue(this->tx_queue_, AXIO_FLOW_SIZE);
      #endif
      /// Dispatch stage
      size_t s_tick = rdtsc();
      size_t nb_collect = 0;
      nb_collect = this->dispatcher_->collect_tx_packets();
      if (AXIO_LIKELY(nb_collect != 0)) {
        net_stats_disp_tx(nb_collect);
        net_stats_disp_tx_duration(s_tick);
      }
      #ifdef AXIO_ONE_STAGE
        this->tx_queue_->reset_head();
        this->dispatcher_->set_tx_queue_index(0);
      #endif
      uint32_t usage = this->dispatcher_->used_buffer_count();
      net_stats_mbuf_usage(usage);
    }

    void nic_tx() {
      #ifdef AXIO_ONE_STAGE
        this->dispatcher_->fill_tx_pkts(AXIO_FLOW_SIZE, kAppReqPayloadSize + 42);
      #endif
      /// Calculate NIC-transmitted packets and duration first.
      size_t nb_tx = 0;
      if (this->dispatcher_->tx_queue_size() >= this->dispatcher_->tx_batch_size()) {
        size_t s_tick = rdtsc();
        nb_tx = this->dispatcher_->flush_tx();
        // AXIO_INFO("Workspace %u successfully transmit %lu packets\n", this->ws_id_, nb_tx);
        net_stats_nic_tx(nb_tx);
        net_stats_disp_tx_stall_duration(s_tick);
      }
    }

    /**
     * @brief Dispatcher rx phase: receive packets from the nic and put them into the 
     * dispatcher rx queue. Drop occurs when the ws queue is full.
    */
    void bursted_rx() {
      #ifdef AXIO_ONE_STAGE
        if (this->queue_empty_) {
          this->dispatcher_->fill_rx_pkts(kWsQueueSize);
          this->dispatcher_->set_rx_queue_index(0);
          this->queue_empty_ = false;
        }
        size_t index = this->dispatcher_->get_rx_queue_index();
        this->dispatcher_->set_rx_queue_index(index+AXIO_FLOW_SIZE);
      #endif
      size_t queue_size = 0, nb_dispatched = 0;
      queue_size = this->dispatcher_->rx_queue_size();
      if (queue_size != 0) {
        size_t s_tick = rdtsc();
        nb_dispatched = this->dispatcher_->template handle_server_packets<AXIO_RX_PACKET_HANDLER>();
        nb_dispatched += this->dispatcher_->dispatch_rx_packets();
        // AXIO_INFO("Workspace %u successfully dispatch %lu packets\n", this->ws_id_, nb_dispatched);
        net_stats_disp_enqueue_drops(queue_size - nb_dispatched);
        net_stats_disp_rx(nb_dispatched);
        net_stats_disp_rx_duration(s_tick);
      }
      #ifdef AXIO_ONE_STAGE
        this->rx_queue_->reset_tail();
        /// release the mbufs
        // size_t size = this->rx_queue_->size();
        // for (size_t j = 0; j < size; j++) {
        //   this->_deallocate((AXIO_MEMORY_BUFFER_TYPE *)this->rx_queue_->dequeue());
        // }
      #endif
    }

    void nic_rx() {
      size_t s_tick = rdtsc(), cur_desc = this->dispatcher_->rx_used_descriptor_count();
      size_t nb_rx = 0;
      /// Calculate NIC received packets and duration first
      if (cur_desc != Dispatcher::kNumRxRingEntries && cur_desc != this->nic_rx_prev_desc_) {
        net_stats_nic_rx_duration(s_tick, this->nic_rx_prev_tick_);
        net_stats_nic_rx(cur_desc, this->nic_rx_prev_desc_);
        double cpt = (double)(s_tick - this->nic_rx_prev_tick_) / (double)(cur_desc - this->nic_rx_prev_desc_);
        net_stats_nic_rx_cpt(cpt);
      }
      nb_rx = this->dispatcher_->receive_burst();
      this->nic_rx_prev_tick_ = rdtsc();
      this->nic_rx_prev_desc_ = this->dispatcher_->rx_used_descriptor_count();
      if (AXIO_LIKELY(nb_rx)){
        // AXIO_INFO("Workspace %u successfully receive %lu packets\n", this->ws_id_, nb_rx);
        net_stats_disp_rx_stall_duration(s_tick); 
      }
      #ifdef AXIO_ONE_STAGE
        this->dispatcher_->free_rx_queue();
      #endif
    }
    
  /**
   * ----------------------User defined methods----------------------
   */ 

 private:
  void _handle_client_messages(AXIO_MEMORY_BUFFER_TYPE** msg, size_t msg_num) {
  #if AXIO_ENABLE_INFLIGHT_LIMIT
    ws_hdr *recv_ws_hdr = this->_extract_workspace_header(msg[0]);
    this->tx_rule_table_->release_inflight_budget(recv_ws_hdr->workload_type_, msg_num);
  #endif
    this->_deallocate_bulk(msg, msg_num * kAppResponsePktsNum);
  }

  /**
   * @brief Wrap a user-defined emulated message handler from msg_handlers.cc.
   * @param msg The messages to be processed
   * @param pkt_num The total number of packets
   */
  template <MessageHandlerType handler>
  void _handle_server_messages(AXIO_MEMORY_BUFFER_TYPE** msg, size_t msg_num);

  /**
   *  \note     T-APP behavior:
   *            [1] recv a huge packet;
   *            [2] scan the huge packet;
   *            [3] free huge packet and apply a new mbuf  
   *            [3] return a small response
   *  \example  distributed file system, e.g., GFS
   */
  void _throughput_intensive_app(AXIO_MEMORY_BUFFER_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr);

  /**
   *  \note     L-APP behavior:
   *            [1] recv a small packet;
   *            [2] scan the small packet;
   *            [3] return a small response
   *  \example  RPC server, e.g., eRPC
   */
  void _latency_intensive_app(AXIO_MEMORY_BUFFER_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr);

  /**
   *  \note     M-APP behavior:
   *            [1] recv a small packet;
   *            [2] scan the small packet;
   *            [3] conduct external memory access;
   *            [4] return a small response
   *  \example  in-memory database, e.g., Redis
   */
  void _memory_intensive_app(AXIO_MEMORY_BUFFER_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr);

  /**
   *  \note     FS-WRITE behavior:
   *            [1] recv a huge packet;
   *            [2] scan the huge packet;
   *            [3] conduct external memory access (from packet to local memory);
   *            [4] return a small response
   */
  void _fs_write(AXIO_MEMORY_BUFFER_TYPE **mbuf_ptr, size_t msg_num, size_t pkt_num, udphdr *uh, ws_hdr *hdr);

  /**
   *  \note     FS-READ behavior:
   *            [1] recv a small packet;
   *            [2] scan the small packet;
   *            [3] conduct external memory access (from local memory to packet);
   *            [4] return a huge response
   */
  void _fs_read(AXIO_MEMORY_BUFFER_TYPE **mbuf_ptr, size_t msg_num, udphdr *uh, ws_hdr *hdr);

  /**
   *  \note     KV behavior:
   *            [1] ;
   *            [2] ;
   *            [3] ;
   *            [4]
   */
  void _handle_kv(AXIO_MEMORY_BUFFER_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr);

  /**
   * ----------------------Util methods----------------------
   */ 
  /// Sync workspaces within the same workspace context.
  void _wait() {
    rt_assert(this->context_ != nullptr, "Workspace is not registered!");
    this->context_->barrier_->wait();
  }

  void _fill_queue(LockFreeQueue* queue, size_t fill_size) {
    if (queue->size() == fill_size) return;
    size_t retry_counter = 0;
    // rt_assert(queue->size() == 0, "filling queue begin with non-empty queue");
    for (size_t i = 0; i < fill_size; i++) {
      AXIO_MEMORY_BUFFER_TYPE* buffer = this->_allocate();
      while (AXIO_UNLIKELY(buffer == nullptr)) {
        buffer = this->_allocate();
        retry_counter++;
        if (!(retry_counter % 100000000)) {
          printf("retry counter = %ld\n", retry_counter);
        }
      }
    #ifdef AXIO_DPDK_MODE
      mbuf_push_data(buffer, kAppLastPaddingSize + 56);
    #else
      buffer->set_length(kAppLastPaddingSize + 56);
    #endif
      queue->enqueue((uint8_t*)buffer);
    }
  }

  /**
   * @brief Allocate one buffer from the registered dispatcher allocator.
   */
  AXIO_MEMORY_BUFFER_TYPE* _allocate() {
    return this->mem_reg_->alloc_(this->mem_reg_->dispatcher_mr_);
  }

  /**
   * @brief Allocate multiple buffers into a caller-owned array.
   * @param buffers Destination array for the allocated buffers.
   * @param count Number of buffers to allocate.
   */
  uint8_t _allocate_bulk(AXIO_MEMORY_BUFFER_TYPE** buffers, size_t count) {
    return this->mem_reg_->alloc_bulk_(
        this->mem_reg_->dispatcher_mr_, buffers, count);
  }

  void _deallocate(AXIO_MEMORY_BUFFER_TYPE* buffer) {
    this->mem_reg_->de_alloc_(buffer, this->mem_reg_->dispatcher_mr_);
  }

  void _deallocate_bulk(AXIO_MEMORY_BUFFER_TYPE** buffers, size_t count) {
    this->mem_reg_->de_alloc_bulk_(
        buffers, count, this->mem_reg_->dispatcher_mr_);
  }

  void _write_payload(AXIO_MEMORY_BUFFER_TYPE* buffer, char* udp_header,
                      char* workspace_header, size_t payload_size) {
    this->mem_reg_->set_payload_(
        buffer, udp_header, workspace_header, payload_size);
  }

  void _copy_payload(AXIO_MEMORY_BUFFER_TYPE* destination, AXIO_MEMORY_BUFFER_TYPE* source,
                     char* udp_header, char* workspace_header,
                     size_t payload_size) {
    this->mem_reg_->cp_payload_(
        destination, source, udp_header, workspace_header, payload_size);
  }

  void _scan_payload(AXIO_MEMORY_BUFFER_TYPE* buffer, size_t payload_size) {
    #ifdef AXIO_DPDK_MODE
      for (uint32_t i = 0; i < buffer->data_len; i++) {
        this->mbuf_data_one_byte_ = rte_pktmbuf_mtod(buffer, uint8_t*)[i];
      }
    #elif defined(AXIO_ROCE_MODE)
      for (uint32_t i = 0; i < buffer->length_; i++) {
        this->mbuf_data_one_byte_ = buffer->buf_[i];
      }
    #endif
  }

  void _read_payload(AXIO_MEMORY_BUFFER_TYPE* buffer, size_t begin, char* destination,
                     size_t copy_size) {
    #ifdef AXIO_DPDK_MODE
      rt_assert(copy_size < buffer->data_len,
                "mbuf payload is smaller than payload needed!");
      memcpy(destination, rte_pktmbuf_mtod(buffer, uint8_t*) + begin,
             copy_size);
    #elif defined(AXIO_ROCE_MODE)
      rt_assert(copy_size < buffer->length_,
                "mbuf payload is smaller than payload needed!");
      memcpy(destination, &(buffer->buf_[begin]), copy_size);
    #endif
  }

  ws_hdr* _extract_workspace_header(AXIO_MEMORY_BUFFER_TYPE* buffer) {
    return this->mem_reg_->extract_ws_hdr_(buffer);
  }

  size_t _rx_ring_size() {
    return this->dispatcher_->kNumRxRingEntries;
  }

  uint8_t _id() {
    return this->ws_id_;
  }

  uint8_t _type() {
    return this->ws_type_;
  }

  uint8_t _workload_type() {
    rt_assert(this->workload_type_ != kInvalidWorkloadType,
              "This workspace has no workload type!");
    return this->workload_type_;
  }

  double _frequency_ghz() {
    return this->freq_ghz_;
  }

  /**
   * @brief Get the memory-region information from this workspace's dispatcher.
   * @throw runtime_error if workspace is not a dispatcher
   */
  Dispatcher::MemoryRegionInfo<AXIO_MEMORY_BUFFER_TYPE>* _memory_region() {
    rt_assert(this->ws_type_ & DISPATCHER,
              "Cannot get memory region, invalid workspace type");
    return this->dispatcher_->memory_region();
  }
  
  /**
   * ----------------------Internal Parameters----------------------
   */
  /// Tx/Rx queues in application level
  LockFreeQueue* rx_queue_ = new LockFreeQueue();
  LockFreeQueue* tx_queue_ = new LockFreeQueue();

  /// Tx/Rx mbuf buffer
  AXIO_MEMORY_BUFFER_TYPE* tx_mbuf_buffer_[kWsQueueSize] = {nullptr};
  AXIO_MEMORY_BUFFER_TYPE* rx_mbuf_buffer_[kWsQueueSize] = {nullptr};

  /// Parameters for single-stage testing
  bool queue_empty_ = true;
  WsContext* context_ = nullptr;
  const uint8_t ws_id_;
  const uint8_t ws_type_;     // dispatcher (2b'01), worker (2b'10), or both
  const uint8_t numa_node_;
  const uint8_t phy_port_;

  /// Parameters for pipeline
  std::vector<phase_t>* ws_loop_ = nullptr;

  /// Application-related parameters
  Dispatcher::MemoryRegionInfo<AXIO_MEMORY_BUFFER_TYPE>* mem_reg_ = nullptr;
  bool inflight_budget_acquired_ = false;
  AXIO_MEMORY_BUFFER_TYPE* tx_mbuf_[kAppRequestPktsNum * kMaxBatchSize] = {nullptr};
  uint8_t workload_type_ = kInvalidWorkloadType;
  uint8_t dispatcher_ws_id_ = kInvalidWsId;
  RuleTable* tx_rule_table_ = new RuleTable();

  /// Stateful memory accessed per packet
  void* stateful_memory_ = nullptr;
  uint64_t stateful_memory_index_ = 0;

  /// Dispatcher-related parameters
  TDispatcher* dispatcher_ = nullptr;

  /// Statistical parameters
  double freq_ghz_ = 0.0;
  struct net_stats* stats_ = new struct net_stats();
  bool stats_init_ws_ = false;
  size_t nic_rx_prev_tick_ = 0;
  size_t nic_rx_prev_desc_ = 0;
  size_t latency_samples_[AXIO_LATENCY_SAMPLE_COUNT] = {0};
  size_t latency_sample_index_ = 0;

  /// Key-value store instance
  KV* kv_store_ = nullptr;

  /**
   * ----------------------Internal Methods----------------------
   */
  /* ----------------------For init---------------------- */
  /**
   * @brief Register the current workspace with its workspace context.
   * @throw runtime_error if workspace is already registered
   */
  void _register();
  void _set_memory_region();
  void _configure_dispatcher();

  /* ----------------------For statistics---------------------- */
  void _update_stats(uint8_t duration);
  void _aggregate_stats(perf_stats* global_stats, double frequency_ghz,
                        uint8_t duration);

  /* ----------------------DEBUG----------------------*/
  uint8_t mbuf_data_one_byte_ = 0;
};

/**
 * ----------------------For template instantiation----------------------
 */
#ifdef AXIO_ROCE_MODE
  #define AXIO_FORCE_COMPILE_DISPATCHER template class Workspace<RoceDispatcher>;
#elif AXIO_DPDK_MODE
  #define AXIO_FORCE_COMPILE_DISPATCHER template class Workspace<DpdkDispatcher>;
#endif
}  // namespace axio
