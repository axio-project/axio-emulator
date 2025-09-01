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

namespace dperf{
/**
 * ----------------------General definations----------------------
 */ 
#define DISPATCHER 1
#define WORKER 2
#define NIC_OFFLOAD 4
#define DISPATCHER_AND_WORKER 3

using phase_t = void (Workspace<DISPATCHER_TYPE>::*)();

template <class TDispatcher>
class Workspace {
  /**
   * ----------------------Parameters tuned by Axio----------------------
   */ 
  uint16_t kAppTxMsgBatchSize = 0;
  uint16_t kAppRxMsgBatchSize = 0;

  /**
   * ----------------------Parameters in Application level----------------------
   */ 
  /// TX specific
  static constexpr size_t kAppRequestPktsNum = ceil((double)kAppReqPayloadSize / (double)Dispatcher::kMaxPayloadSize);  // number of packets in a request message
  static constexpr size_t kAppFullPaddingSize = Dispatcher::kMaxPayloadSize - sizeof(ws_hdr);
  static constexpr size_t kAppLastPaddingSize = kAppReqPayloadSize - (kAppRequestPktsNum - 1) * Dispatcher::kMaxPayloadSize - sizeof(ws_hdr);
  // RX specific
  static constexpr size_t kAppReponsePktsNum = ceil((double)kAppRespPayloadSize / (double)Dispatcher::kMaxPayloadSize); // number of packets in a response message
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
              std::vector<dperf::phase_t> *ws_loop, UserConfig *user_config);
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
    #if EnableInflyMessageLimit
      // we block until we have infly budget
      if(tx_rule_table_->apply_infly_budget(workload_type_, kAppTxMsgBatchSize) == false){
        infly_flag_ = false;
        return;
      }
      infly_flag_ = true;
    #endif

      size_t s_tick = rdtsc();
      while (unlikely(alloc_bulk(tx_mbuf_, kAppRequestPktsNum * kAppTxMsgBatchSize) != 0)) {
        net_stats_app_apply_mbuf_stalls();
      }

      net_stats_app_tx_stall_duration(s_tick);

      // zhuobin: we will measure the usgae of mempool under OneStage mode, to dignose the reason of 
      //          stall, either (1) too many conflict due to small batch size, or (2) mempool is 
      //          congestion.
      //          remember to make sure the dispatcher thread has app workload!
    #ifdef OneStage
      if(ws_type_ & DISPATCHER){
        uint32_t usage = dispatcher_->get_used_mbuf_num();
        net_stats_mbuf_usage(usage);
      }
    #endif
    }

    /**
     * @brief App tx phase, step 2: generate packets. Drop occurs when the tx queue is full
    */
    void generate_pkts() {
      #if EnableInflyMessageLimit
        if(!infly_flag_) return;
      #endif
      size_t s_tick = rdtsc();
      /// partially set udp header
      udphdr uh;
      uh.source = ws_id_;
      uh.dest = tx_rule_table_->rr_select(workload_type_);
      /// set workspace header
      ws_hdr hdr;
      hdr.workload_type_ = workload_type_;
      hdr.segment_num_ = kAppRequestPktsNum;
      MEM_REG_TYPE **mbuf_ptr = tx_mbuf_;
      /// Insert payload to mbufs
      for (size_t msg_idx = 0; msg_idx < kAppTxMsgBatchSize; msg_idx++) {
        /// TBD: Perform extra memory access and calculation for each message
        /// Iterate all messages in a batch
        for (size_t seg_idx = 0; seg_idx < kAppRequestPktsNum - 1; seg_idx++) {
          /// Iterate all segments in a message
          set_payload(*mbuf_ptr, (char*)&uh, (char*)&hdr, kAppFullPaddingSize);
          mbuf_ptr++;
        }
        set_payload(*mbuf_ptr, (char*)&uh, (char*)&hdr, kAppLastPaddingSize);
        mbuf_ptr++;
      }
      /// Insert packets to worker tx queue
      size_t drop_num = 0;
      for (size_t i = 0; i < kAppRequestPktsNum * kAppTxMsgBatchSize; i++) {
        if (unlikely(!tx_queue_->enqueue((uint8_t*)tx_mbuf_[i]))) {
          /// Drop the packet if the tx queue is full
          de_alloc(tx_mbuf_[i]);
          drop_num++;
        }
      }
      net_stats_app_tx(kAppTxMsgBatchSize * kAppRequestPktsNum - drop_num);
      net_stats_app_drops(drop_num);
      net_stats_app_tx_duration(s_tick);
      #ifdef OneStage
        tx_queue_->reset_tail();
        s_tick = rdtsc();
        de_alloc_bulk(tx_mbuf_, kAppRequestPktsNum * kAppTxMsgBatchSize);
        net_stats_app_tx_stall_duration(s_tick);
        // for (size_t i = 0; i < kAppRequestPktsNum * kAppTxMsgBatchSize; i++) {
        //   de_alloc(tx_mbuf_[i]);
        // }
      #endif
    }

    /**
     * @brief App rx phase: handle received messages. 
    */
    void app_handler() {
    #ifdef OneStage
        fill_queue(rx_queue_, FlowSize);
    #endif
      size_t s_tick = rdtsc();
      size_t rx_size = rx_queue_->get_size();

      /**
       *  @brief  mock the process of handling one single meesage
       *  @param  msg       pointer to the message to be processed
       *  @param  ticks     specified processing ticks
      */
      auto __mock_process_msg = [&](MEM_REG_TYPE** msg, uint64_t ticks, size_t msg_num) {
        uint64_t s_tick, passed_ticks = 0;

        s_tick = rdtsc();
        // step 1: exec message processing handler
        #if NODE_TYPE == CLIENT
          this->msg_handler_client(msg, msg_num);
        #else
          this->template msg_handler_server<kRxMsgHandler>(msg, msg_num);
        #endif
  
        // step 2: mock remain ticks
        do {
          passed_ticks = rdtsc() - s_tick;
        } while(passed_ticks < ticks);
      };

      /// enter rule, receive >= kAppRxMsgBatchSize requests to process
    #if NODE_TYPE == CLIENT
      size_t msg_num = rx_size / kAppReponsePktsNum;
      if (msg_num < kAppRxMsgBatchSize)
        return;
      /// handle message
      for (size_t i = 0; i < msg_num; i++) {
        for (size_t j = 0; j < kAppReponsePktsNum; j++) {
          rx_mbuf_buffer_[i*kAppReponsePktsNum + j] = (MEM_REG_TYPE*)rx_queue_->dequeue();
          rt_assert(rx_mbuf_buffer_[i*kAppReponsePktsNum + j] != nullptr, "Get invalid mbuf!");
        }
      }
      __mock_process_msg(rx_mbuf_buffer_, kAppTicksPerMsg * msg_num, msg_num);
      net_stats_app_rx(msg_num * kAppReponsePktsNum); // 
    #else
      size_t msg_num = rx_size / kAppRequestPktsNum;
      if (msg_num < kAppRxMsgBatchSize)
        return;
      /// handle message
      for (size_t i = 0; i < msg_num; i++) {
        for (size_t j = 0; j < kAppRequestPktsNum; j++) {
          rx_mbuf_buffer_[i*kAppRequestPktsNum + j] = (MEM_REG_TYPE*)rx_queue_->dequeue();
          rt_assert(rx_mbuf_buffer_[i*kAppRequestPktsNum + j] != nullptr, "Get invalid mbuf!");
        }
      }
      __mock_process_msg(rx_mbuf_buffer_, kAppTicksPerMsg * msg_num, msg_num);
      net_stats_app_rx(msg_num * kAppRequestPktsNum);
    #endif
      net_stats_app_rx_duration(s_tick);

      #ifdef OneStage
        size_t size = tx_queue_->get_size();
        for (size_t j = 0; j < size; j++) {
          // de_alloc((MEM_REG_TYPE *)tx_queue_->dequeue());
          rx_queue_->enqueue(tx_queue_->dequeue());
        }
      #endif
    }

    /**
     * @brief Dispatcher tx phase: collect packets from worker tx queue and flush to the nic. 
     * Stall occurs when the tx ring is full.
    */
    void bursted_tx() {
      #ifdef OneStage
        fill_queue(tx_queue_, FlowSize);
      #endif
      /// Dispatch stage
      size_t s_tick = rdtsc();
      size_t nb_collect = 0;
      nb_collect = dispatcher_->collect_tx_pkts();
      if (likely(nb_collect != 0)) {
        net_stats_disp_tx(nb_collect);
        net_stats_disp_tx_duration(s_tick);
      }
      #ifdef OneStage
        tx_queue_->reset_head();
        dispatcher_->set_tx_queue_index(0);
      #endif
      uint32_t usage = dispatcher_->get_used_mbuf_num();
      net_stats_mbuf_usage(usage);
    }

    void nic_tx() {
      #ifdef OneStage
        dispatcher_->fill_tx_pkts(FlowSize, kAppReqPayloadSize + 42);
      #endif
      /// Calculate NIC transimitted packets and duration first
      size_t nb_tx = 0;
      if (dispatcher_->get_tx_queue_size() >= dispatcher_->kDispTxBatchSize) {
        size_t s_tick = rdtsc();
        nb_tx = dispatcher_->tx_flush();
        // DPERF_INFO("Workspace %u successfully transmit %lu packets\n", ws_id_, nb_tx); 
        net_stats_nic_tx(nb_tx);
        net_stats_disp_tx_stall_duration(s_tick);
      }
    }

    /**
     * @brief Dispatcher rx phase: receive packets from the nic and put them into the 
     * dispatcher rx queue. Drop occurs when the ws queue is full.
    */
    void bursted_rx() {
      #ifdef OneStage
        if (queue_empty) {
          dispatcher_->fill_rx_pkts(kWsQueueSize);
          dispatcher_->set_rx_queue_index(0);
          queue_empty = false;
        }
        size_t index = dispatcher_->get_rx_queue_index();
        dispatcher_->set_rx_queue_index(index+FlowSize);
      #endif
      size_t queue_size = 0, nb_dispatched = 0;
      queue_size = dispatcher_->get_rx_queue_size();
      if (queue_size != 0) {
        size_t s_tick = rdtsc();
        nb_dispatched = dispatcher_->template pkt_handler_server<kRxPktHandler>();
        nb_dispatched += dispatcher_->dispatch_rx_pkts();
        // DPERF_INFO("Workspace %u successfully dispatch %lu packets\n", ws_id_, nb_dispatched);
        net_stats_disp_enqueue_drops(queue_size - nb_dispatched);
        net_stats_disp_rx(nb_dispatched);
        net_stats_disp_rx_duration(s_tick);
      }
      #ifdef OneStage
        rx_queue_->reset_tail();
        /// release the mbufs
        // size_t size = rx_queue_->get_size();
        // for (size_t j = 0; j < size; j++) {
        //   de_alloc((MEM_REG_TYPE *)rx_queue_->dequeue());
        // }
      #endif
    }

    void nic_rx(){
      size_t s_tick = rdtsc(), cur_desc = dispatcher_->get_rx_used_desc();
      size_t nb_rx = 0;
      /// Calculate NIC received packets and duration first
      if (cur_desc != Dispatcher::kNumRxRingEntries && cur_desc != nic_rx_prev_desc_) {
        net_stats_nic_rx_duration(s_tick, nic_rx_prev_tick_);
        net_stats_nic_rx(cur_desc, nic_rx_prev_desc_);
        double cpt = (double)(s_tick - nic_rx_prev_tick_) / (double)(cur_desc - nic_rx_prev_desc_);
        net_stats_nic_rx_cpt(cpt);
      }
      nb_rx = dispatcher_->rx_burst();
      nic_rx_prev_tick_ = rdtsc();
      nic_rx_prev_desc_ = dispatcher_->get_rx_used_desc();
      if (likely(nb_rx)){
        // DPERF_INFO("Workspace %u successfully receive %lu packets\n", ws_id_, nb_rx);
        net_stats_disp_rx_stall_duration(s_tick); 
      }
      #ifdef OneStage
        dispatcher_->free_rx_queue();
      #endif
    }
    
  /**
   * ----------------------User defined methods----------------------
   */ 

  void msg_handler_client(MEM_REG_TYPE** msg, size_t msg_num) {
  #if EnableInflyMessageLimit
    ws_hdr *recv_ws_hdr = extract_ws_hdr(msg[0]);
    tx_rule_table_->return_infly_budget(recv_ws_hdr->workload_type_, msg_num);
  #endif
    de_alloc_bulk(msg, msg_num * kAppReponsePktsNum);
  }

  /**
   * @brief message handler wrapper, user-defined emlated message handlers are defined at msg_handler.cc
   * @param msg The messages to be processed
   * @param pkt_num The total number of packets
   */
  template <msg_handler_type_t handler>
  void msg_handler_server(MEM_REG_TYPE** msg, size_t msg_num);

  /**
   *  \note     T-APP behavior:
   *            [1] recv a huge packet;
   *            [2] scan the huge packet;
   *            [3] free huge packet and apply a new mbuf  
   *            [3] return a small response
   *  \example  distributed file system, e.g., GFS
   */
  void throughput_intense_app(MEM_REG_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr);

  /**
   *  \note     L-APP behavior:
   *            [1] recv a small packet;
   *            [2] scan the small packet;
   *            [3] return a small response
   *  \example  RPC server, e.g., eRPC
   */
  void latency_intense_app(MEM_REG_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr);

  /**
   *  \note     M-APP behavior:
   *            [1] recv a small packet;
   *            [2] scan the small packet;
   *            [3] conduct external memory access;
   *            [4] return a small response
   *  \example  in-memory database, e.g., Redis
   */
  void memory_intense_app(MEM_REG_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr);

  /**
   *  \note     FS-WRITE behavior:
   *            [1] recv a huge packet;
   *            [2] scan the huge packet;
   *            [3] conduct external memory access (from packet to local memory);
   *            [4] return a small response
   */
  void fs_write(MEM_REG_TYPE **mbuf_ptr, size_t msg_num, size_t pkt_num, udphdr *uh, ws_hdr *hdr);

  /**
   *  \note     FS-READ behavior:
   *            [1] recv a small packet;
   *            [2] scan the small packet;
   *            [3] conduct external memory access (from local memory to packet);
   *            [4] return a huge response
   */
  void fs_read(MEM_REG_TYPE **mbuf_ptr, size_t msg_num, udphdr *uh, ws_hdr *hdr);

  /**
   *  \note     KV behavior:
   *            [1] ;
   *            [2] ;
   *            [3] ;
   *            [4]
   */
  void kv_handler(MEM_REG_TYPE **mbuf_ptr, size_t pkt_num, udphdr *uh, ws_hdr *hdr);

  /**
   * ----------------------Util methods----------------------
   */ 
  public:
    /// Sync workspaces within the same workspace context
    void wait() {
      rt_assert(context_ != nullptr, "Workspace is not registered!");
      context_->barrier_->wait();
    }

    void fill_queue(lock_free_queue* queue, size_t fill_size) {
        if (queue->get_size() == fill_size) return;
        size_t retry_counter = 0;
        // rt_assert(queue->get_size() == 0, "filling queue begin with non-empty queue");
        for (size_t i = 0; i < fill_size; i++) {
            MEM_REG_TYPE* temp_mbuf = alloc();
            while(unlikely(temp_mbuf == NULL)) {
              temp_mbuf = alloc();
              retry_counter++;
              if(!(retry_counter%100000000)) printf("retry counter = %ld\n", retry_counter);
            }
          #ifdef DpdkMode
            mbuf_push_data(temp_mbuf, kAppLastPaddingSize+56);
          #else
            temp_mbuf->set_length(kAppLastPaddingSize+56);
          #endif
            queue->enqueue((uint8_t*)temp_mbuf);
        }
    }
    /**
     * @brief Methods to allocate/de-allocate/modify mbufs
     * @param mbuf The mbuf is defined by the dispatcher
     * @param mem_reg_ Registered by the dispatcher
    */
    MEM_REG_TYPE * alloc() {
      MEM_REG_TYPE *mbuf = mem_reg_->alloc_(mem_reg_->dispatcher_mr_);
      return mbuf;
    }

    /**
     * @brief Methods to allocate/de-allocate multiple mbufs (virtual addr is continuous)
     * @param mbuf The mbuf is defined by the dispatcher
     * @param mem_reg_ Registered by the dispatcher
    */
    uint8_t alloc_bulk(MEM_REG_TYPE **m, size_t num) {
      uint8_t res = mem_reg_->alloc_bulk_(mem_reg_->dispatcher_mr_, m, num);
      return res;
    }

    void de_alloc(MEM_REG_TYPE *mbuf) {
      mem_reg_->de_alloc_(mbuf, mem_reg_->dispatcher_mr_);
    }

    void de_alloc_bulk(MEM_REG_TYPE **m, size_t num) {
      mem_reg_->de_alloc_bulk_(m, num, mem_reg_->dispatcher_mr_);
    }

    void set_payload(MEM_REG_TYPE *mbuf, char* udp_header, char* ws_header, size_t payload_size) {
      mem_reg_->set_payload_(mbuf, udp_header, ws_header, payload_size);
    }

    void cp_payload(MEM_REG_TYPE *dst_mbuf, MEM_REG_TYPE *src_mbuf, char* udp_header, char* ws_header, size_t payload_size) {
      mem_reg_->cp_payload_(dst_mbuf, src_mbuf, udp_header, ws_header, payload_size);
    }

    void scan_payload(MEM_REG_TYPE *m, size_t payload_size){
    #ifdef DpdkMode
      for (uint32_t i = 0; i < m->data_len; i++) {
          mbuf_data_one_byte_ = rte_pktmbuf_mtod(m, uint8_t *)[i];
      }
    #elif defined(RoceMode)
      for (uint32_t i = 0; i < m->length_; i++) {
          mbuf_data_one_byte_ = m->buf_[i];
      }
    #endif
    }
    void get_payload(MEM_REG_TYPE *m, size_t begin, char* dst, size_t cp_size) {
      #ifdef DpdkMode
      rt_assert(cp_size < m->data_len, "mbuf payload is smaller than payload needed!");
        memcpy(dst, rte_pktmbuf_mtod(m, uint8_t *) + begin, cp_size);
      #elif defined(RoceMode)
        rt_assert(cp_size < m->length_, "mbuf payload is smaller than payload needed!");
        memcpy(dst, &(m->buf_[begin]), cp_size);
      #endif
    }

    ws_hdr* extract_ws_hdr(MEM_REG_TYPE *mbuf){
      return mem_reg_->extract_ws_hdr_(mbuf);
    }
    
    size_t get_RX_ring_size() {
      return dispatcher_->kNumRxRingEntries;
    }

    uint8_t get_ws_id() {
      return ws_id_;
    }

    uint8_t get_ws_type() {
      return ws_type_;
    }

    uint8_t get_workload_type() {
      rt_assert(workload_type_ != kInvalidWorkloadType, "This workspace has no workload type!");
      return workload_type_;
    }

    double get_freq() {
      return freq_ghz_;
    }

    /**
     * @brief Get the memory_region_info from workspace->dispatcher
     * @throw runtime_error if workspace is not a dispatcher
    */
    Dispatcher::mem_reg_info<MEM_REG_TYPE> * get_mem_reg() {
      rt_assert(ws_type_ & DISPATCHER, "Cannot get memory region, invalid workspace type");
      return dispatcher_->get_mem_reg();
    }
  
  /**
   * ----------------------Internal Parameters----------------------
   */
  public:
    /// Tx/Rx queues in application level
    lock_free_queue* rx_queue_ = new lock_free_queue();
    lock_free_queue* tx_queue_ = new lock_free_queue();

    /// Tx/Rx mbuf buffer
    MEM_REG_TYPE *tx_mbuf_buffer_[kWsQueueSize] = {nullptr};
    MEM_REG_TYPE *rx_mbuf_buffer_[kWsQueueSize] = {nullptr};

    /// Parameters for Singe Stage Test
    bool queue_empty = true;
  private:   
    WsContext *context_ = nullptr;
    const uint8_t ws_id_;
    const uint8_t ws_type_;     // ws type: dispatcher (2b'01), worker (2b'10), or both (2b'11)
    const uint8_t numa_node_;   // numa node that the workspace is located
    const uint8_t phy_port_;    // datapath physical port, typically refers to a NIC port
    // size_t loop_tsc_ = 0;

    /// Parameters for pipeline
    std::vector<phase_t> *ws_loop_ = nullptr;
    /// Application related parameters
    Dispatcher::mem_reg_info<MEM_REG_TYPE> *mem_reg_ = nullptr;     // registered by the dispatcher
    bool infly_flag_ = false;
    MEM_REG_TYPE *tx_mbuf_[kAppRequestPktsNum * kMaxBatchSize] = {nullptr};
    uint8_t workload_type_ = kInvalidWorkloadType; 
    uint8_t dispatcher_ws_id_ = kInvalidWsId;                  // A group of worker workspaces only have one dispatcher
    RuleTable *tx_rule_table_ = new RuleTable();

    /// Stateful memory accessed per packet
    void *stateful_memory_;
    uint64_t stateful_memory_access_ptr_;

    /// Dispatcher related parameters
    TDispatcher *dispatcher_ = nullptr;

    /// Statistical parameters
    double freq_ghz_ = 0.0;
    struct net_stats *stats_ = new struct net_stats();
    bool stats_init_ws_ = false;
    size_t nic_rx_prev_tick_ = 0, nic_rx_prev_desc_ = 0;
    size_t lat_sample_vector[PERF_LAT_SAMPLE_NUM] = {0};
    size_t lat_sample_idx = 0;

    // key-value store instance
    KV* kv;

  /**
   * ----------------------Internal Methods----------------------
   */
  private:
    /* ----------------------For init---------------------- */
    /**
     * @brief Register current workspace to workspace_context
     * 
     * @throw runtime_error if workspace is already registered
    */
    void register_ws();
    void set_mem_reg();
    void set_dispatcher_config();

    /* ----------------------For statistics---------------------- */
    void update_stats(uint8_t duration);
    void aggregate_stats(perf_stats *g_stats, double freq, uint8_t duration);

    /* ----------------------DEBUG----------------------*/
    uint8_t mbuf_data_one_byte_ = 0;
};

/**
 * ----------------------For template instantiation----------------------
 */
#ifdef RoceMode
  #define FORCE_COMPILE_DISPATCHER template class Workspace<RoceDispatcher>;
#elif DpdkMode
  #define FORCE_COMPILE_DISPATCHER template class Workspace<DpdkDispatcher>;
#endif
}
