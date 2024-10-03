/**
 * @file workspace.cc
 * @brief Executing workspace of a datapath pipeline. In theory, workspace can
 * be called by various thread library such as pthread, datapath OS, DOCA, etc.
 */
#include "workspace.h"

namespace dperf {

template <class TDispatcher>
Workspace<TDispatcher>::Workspace(WsContext *context, uint8_t ws_id, uint8_t ws_type, 
                                  uint8_t numa_node, uint8_t phy_port, 
                                  std::vector<dperf::phase_t> *ws_loop,
                                  UserConfig *user_config)
    : context_(context),
      ws_id_(ws_id),
      ws_type_(ws_type),
      numa_node_(numa_node),
      phy_port_(phy_port),
      ws_loop_(ws_loop) {

  if (ws_type_ == 0) {
    DPERF_INFO("Workspace %u is not used\n", ws_id_);
    return;
  }

  // Parameter Check
  rt_assert(ws_type_ != kInvaildWorkspaceType, "Invalid workspace type");
  rt_assert(phy_port < kMaxPhyPorts, "Invalid physical port");
  rt_assert(numa_node_ < kMaxNumaNodes, "Invalid NUMA node");

  // Init and check tunable parameters
  rt_assert(user_config->tune_params_ != nullptr, "Tunable parameters are not loaded");
  rt_assert(user_config->tune_params_->kAppCoreNum <= kWorkspaceMaxNum, "App core number is too large");
  kAppTxMsgBatchSize = user_config->tune_params_->kAppTxMsgBatchSize;
  rt_assert(kAppTxMsgBatchSize <= kMaxBatchSize, "App TX batch size is too large");
  kAppRxMsgBatchSize = user_config->tune_params_->kAppRxMsgBatchSize;
  rt_assert(kAppRxMsgBatchSize <= kMaxBatchSize, "App RX batch size is too large");

  /* Init workspace, phase 1 */
  if (ws_type_ & WORKER) {
    workload_type_ = user_config->workloads_config_->ws_id_workload_map[ws_id_];
    uint8_t group_idx = user_config->workloads_config_->ws_id_group_idx_map[ws_id_];
    dispatcher_ws_id_ = user_config->workloads_config_->workload_dispatcher_map[workload_type_][group_idx];
    /// config tx rule table
    for (auto &remote_dispatcher_ws_id : user_config->workloads_config_->workload_remote_dispatcher_map[workload_type_]) {
      tx_rule_table_->add_route(workload_type_, remote_dispatcher_ws_id);
    }
    printf("Workspace %u is assigned to workload %u, dispatcher %u\n", ws_id_, workload_type_, dispatcher_ws_id_);

    if constexpr (kMemoryAccessRangePerPkt > 0) {
      stateful_memory_ = malloc(kStatefulMemorySizePerCore);
      assert(stateful_memory_ != nullptr);
      memset(stateful_memory_, 'a', kStatefulMemorySizePerCore);
      stateful_memory_access_ptr_ = 0;
    }
  }
  if (ws_type_ & DISPATCHER) {
    dispatcher_ = new TDispatcher(ws_id_, phy_port_, numa_node_, user_config);
  }
  // Register this workspace to ws context. Then, workspace can communicate with
  // each other through ws context.
  register_ws();

  // Wait for all workspaces to be registered
  wait();

  /* Init workspace, phase 2 */
  if (ws_type_ & WORKER) {
    set_mem_reg();
    if (mem_reg_ == nullptr) {
      DPERF_ERROR("Workspace %u cannot get mem_reg\n", ws_id_);
      return;
    }
  }
  if (ws_type_ & DISPATCHER) {
    /// config rx rule table and workspace queues
    set_dispatcher_config();
    if (dispatcher_->get_ws_tx_queue_size() == 0) {
      DPERF_ERROR("Failed to config dispatcher %u\n", ws_id_);
      return;
    }
  }
  wait();   // Force sync before launch
}

template <class TDispatcher>
Workspace<TDispatcher>::~Workspace(){
  DPERF_INFO("Destroying Ws %u.\n", ws_id_);
  delete dispatcher_;
}

template <class TDispatcher>
void Workspace<TDispatcher>::register_ws() {
  std::lock_guard<std::mutex> lock(context_->mutex_);
  rt_assert(context_->ws_[ws_id_] == nullptr, "Workspace already registered!");
  context_->ws_[ws_id_] = this;
  context_->active_ws_id_.push_back(ws_id_);
  if (ws_type_ & WORKER) {
    context_->ws_tx_queue_map_[ws_id_] = tx_queue_;
    context_->ws_rx_queue_map_[ws_id_] = rx_queue_;
    context_->ws_id_dispatcher_map_[ws_id_] = dispatcher_ws_id_;
  }
  if (ws_type_ & DISPATCHER) {
    if (context_->mem_reg_map_.find(ws_id_) != context_->mem_reg_map_.end()) {
      DPERF_ERROR("Dispatcher %u already registered\n", ws_id_);
      return;
    }
    context_->mem_reg_map_.insert(std::make_pair(ws_id_, dispatcher_->get_mem_reg()));
  }
}

template <class TDispatcher>
void Workspace<TDispatcher>::set_mem_reg() {
  std::lock_guard<std::mutex> lock(context_->mutex_);
  mem_reg_ = context_->mem_reg_map_[dispatcher_ws_id_];
}

template <class TDispatcher>
void Workspace<TDispatcher>::set_dispatcher_config() {
  std::lock_guard<std::mutex> lock(context_->mutex_);
  for (auto &ws_id : context_->active_ws_id_) {
    auto it = context_->ws_id_dispatcher_map_.find(ws_id);
    if (it != context_->ws_id_dispatcher_map_.end() && it->second == ws_id_) {
      /// get one worker assigned to this dispatcher
      uint8_t workload_type = context_->ws_[ws_id]->get_workload_type();
      dispatcher_->add_ws_tx_queue(context_->ws_tx_queue_map_[ws_id]);
      dispatcher_->add_ws_rx_queue(ws_id, context_->ws_rx_queue_map_[ws_id]);
      dispatcher_->add_rx_rule(workload_type, ws_id);
    }
  }
}

template <class TDispatcher>
void Workspace<TDispatcher>::launch() {
  for (auto &phase : *ws_loop_) {
    (this->*phase)();
  }
}

template <class TDispatcher>
void Workspace<TDispatcher>::aggregate_stats(perf_stats *g_stats, double freq, uint8_t duration){
  /// App
  double self_app_tx_tp = 0, self_app_rx_tp = 0;
  double self_app_tx_compl = 0, self_app_tx_compl_avg = 0, self_app_tx_compl_min = 0, self_app_tx_compl_max = 0;
  double self_app_tx_stall = 0, self_app_tx_stall_avg = 0, self_app_tx_stall_min = 0, self_app_tx_stall_max = 0;
  double self_app_rx_compl = 0, self_app_rx_compl_avg = 0, self_app_rx_compl_min = 0, self_app_rx_compl_max = 0;
  double self_app_rx_stall = 0, self_app_rx_stall_avg = 0, self_app_rx_stall_min = 0, self_app_rx_stall_max = 0;
  double self_app_rx_batch = 0;

  self_app_tx_tp = (double)stats_->app_tx_msg_num / 1e6 / duration;
  self_app_rx_tp = (double)stats_->app_rx_msg_num / 1e6 / duration;

  if (stats_->app_tx_msg_num) {
    // app tx phrase 2
    self_app_tx_compl = to_usec(stats_->app_tx_avg_duration, freq) / stats_->app_tx_msg_num;
    self_app_tx_compl_avg = to_usec(stats_->app_tx_avg_duration, freq) / stats_->app_tx_invoke_times;
    self_app_tx_compl_min = to_usec(stats_->app_tx_min_duration, freq);
    self_app_tx_compl_max = to_usec(stats_->app_tx_max_duration, freq);

    g_stats->app_tx_compl_ += self_app_tx_compl;
    g_stats->app_tx_compl_max_  = g_stats->app_tx_compl_max_ < self_app_tx_compl_max 
                                ? self_app_tx_compl_max : g_stats->app_tx_compl_max_;
    g_stats->app_tx_compl_min_  = g_stats->app_tx_compl_min_ > self_app_tx_compl_min
                                ? self_app_tx_compl_min : g_stats->app_tx_compl_min_;
    g_stats->app_tx_compl_avg_ += self_app_tx_compl_avg;

    // app tx phrase 1
    self_app_tx_stall = to_usec(stats_->app_tx_stall_avg_duration, freq) / stats_->app_tx_msg_num;
    self_app_tx_stall_avg = to_usec(stats_->app_tx_stall_avg_duration, freq) / stats_->app_tx_invoke_times;
    self_app_tx_stall_min = to_usec(stats_->app_tx_stall_min_duration, freq);
    self_app_tx_stall_max = to_usec(stats_->app_tx_stall_max_duration, freq);
    
    g_stats->app_tx_stall_ += self_app_tx_stall;
    g_stats->app_tx_stall_max_ = g_stats->app_tx_stall_max_ < self_app_tx_stall_max 
                                ? self_app_tx_stall_max : g_stats->app_tx_stall_max_;
    g_stats->app_tx_stall_min_ = g_stats->app_tx_stall_min_ > self_app_tx_stall_min
                                ? self_app_tx_stall_min : g_stats->app_tx_stall_min_;
    g_stats->app_tx_stall_avg_ += self_app_tx_stall_avg;
  }
  if (stats_->app_rx_msg_num ) {
    self_app_rx_batch = (double)stats_->app_rx_msg_num / stats_->app_rx_invoke_times;
    // app rx phrase 2
    self_app_rx_compl = to_usec(stats_->app_rx_avg_duration, freq) / stats_->app_rx_msg_num;
    self_app_rx_compl_avg = to_usec(stats_->app_rx_avg_duration, freq) / stats_->app_rx_invoke_times;
    self_app_rx_compl_min = to_usec(stats_->app_rx_min_duration, freq);
    self_app_rx_compl_max = to_usec(stats_->app_rx_max_duration, freq);

    g_stats->app_rx_compl_ += self_app_rx_compl;
    g_stats->app_rx_compl_max_  = g_stats->app_rx_compl_max_ < self_app_rx_compl_max 
                                ? self_app_rx_compl_max : g_stats->app_rx_compl_max_;
    g_stats->app_rx_compl_min_  = g_stats->app_rx_compl_min_ > self_app_rx_compl_min
                                ? self_app_rx_compl_min : g_stats->app_rx_compl_min_;
    g_stats->app_rx_compl_avg_ += self_app_rx_compl_avg;

    // app rx phrase 1
    self_app_rx_stall = to_usec(stats_->app_rx_stall_avg_duration, freq) / stats_->app_rx_msg_num;
    self_app_rx_stall_avg = to_usec(stats_->app_rx_stall_avg_duration, freq) / stats_->app_rx_invoke_times;
    self_app_rx_stall_min = to_usec(stats_->app_rx_stall_min_duration, freq);
    self_app_rx_stall_max = to_usec(stats_->app_rx_stall_max_duration, freq);
    
    g_stats->app_rx_stall_ += self_app_rx_stall;
    g_stats->app_rx_stall_max_ = g_stats->app_rx_stall_max_ < self_app_rx_stall_max 
                                ? self_app_rx_stall_max : g_stats->app_rx_stall_max_;
    g_stats->app_rx_stall_min_ = g_stats->app_rx_stall_min_ > self_app_rx_stall_min
                                ? self_app_rx_stall_min : g_stats->app_rx_stall_min_;
    g_stats->app_rx_stall_avg_ += self_app_rx_stall_avg;
  }

  /// Dispatcher
  double self_disp_tx_tp = 0, self_disp_rx_tp = 0, self_disp_tx_compl = 0, self_disp_tx_stall = 0, self_disp_rx_compl = 0, self_disp_rx_stall = 0;
  self_disp_tx_tp = (double)stats_->disp_tx_pkt_num / 1e6 / duration;
  self_disp_rx_tp = (double)stats_->disp_rx_pkt_num / 1e6 / duration;
  if (stats_->disp_tx_pkt_num) {
    self_disp_tx_compl = to_usec(stats_->disp_tx_duration, freq) / stats_->disp_tx_pkt_num;
    g_stats->disp_tx_compl_ += self_disp_tx_compl;
    self_disp_tx_stall = to_usec(stats_->disp_tx_stall_duration, freq) / stats_->disp_tx_pkt_num;
    g_stats->disp_tx_stall_ += self_disp_tx_stall;
  }
  if (stats_->disp_rx_pkt_num) {
    self_disp_rx_compl = to_usec(stats_->disp_rx_duration, freq) / stats_->disp_rx_pkt_num;
    g_stats->disp_rx_compl_ += self_disp_rx_compl;
    self_disp_rx_stall = to_usec(stats_->disp_rx_stall_duration, freq) / stats_->disp_rx_pkt_num;
    g_stats->disp_rx_stall_ += self_disp_rx_stall;
  }

  /// NIC
  double self_nic_tx_tp = 0, self_nic_rx_tp = 0, self_nic_tx_compl = 0, self_nic_rx_compl = 0;
  /// nic tx is same with disp tx stall
  self_nic_tx_tp = (double)stats_->nic_tx_pkt_num / 1e6 / duration;
  if (stats_->nic_tx_pkt_num) {
    self_nic_tx_compl = to_usec(stats_->disp_tx_stall_duration, freq) / stats_->nic_tx_pkt_num;
    g_stats->nic_tx_compl_ += self_nic_tx_compl;
  }
  if (stats_->nic_rx_times) {
    self_nic_rx_compl = to_usec(static_cast<size_t>(std::round(stats_->nic_rx_cpt)), freq) / stats_->nic_rx_times;
    g_stats->nic_rx_compl_ += self_nic_rx_compl;
    self_nic_rx_tp = 1.0 / self_nic_rx_compl;
  }

  /// OneStage
  #ifdef OneStage
    double max_tput = FlowSize * kAppRequestPktsNum; //FlowSize*(timeout_tsc/interval_tsc);
    double os_app_tx_tp  = std::min((double)1 / (self_app_tx_compl + self_app_tx_stall),max_tput);
    double os_app_rx_tp  = std::min((double)1 / (self_app_rx_compl + self_app_rx_stall),max_tput);
    double os_disp_tx_tp = std::min((double)1 / (self_disp_tx_compl + self_disp_tx_stall),max_tput);
    double os_disp_rx_tp = std::min((double)1 / (self_disp_rx_compl + self_disp_rx_stall),max_tput);
    double os_nic_tx_tp = std::min((double)1 / (self_nic_tx_compl),max_tput);
    double os_nic_rx_tp = std::min((double)1 / (self_nic_rx_compl),max_tput);
    g_stats->app_tx_throughput_ += os_app_tx_tp; 
    g_stats->app_rx_throughput_ += os_app_rx_tp; 
    g_stats->disp_tx_throughput_ += os_disp_tx_tp;
    g_stats->disp_rx_throughput_ += os_disp_rx_tp;
    g_stats->nic_tx_throughput_ += os_nic_tx_tp;
    g_stats->nic_rx_throughput_ += os_nic_rx_tp;
  #else
    g_stats->app_tx_throughput_ += self_app_tx_tp;
    g_stats->app_rx_throughput_ += self_app_rx_tp;
    g_stats->disp_tx_throughput_ += self_disp_tx_tp;
    g_stats->disp_rx_throughput_ += self_disp_rx_tp;
    g_stats->nic_tx_throughput_ += self_nic_tx_tp;
    g_stats->nic_rx_throughput_ += self_nic_rx_tp;
  #endif

  /// Diagnose Stats for debugging
  // printf("[Workspace %u] bind to core %lu\n", ws_id_, context_->cpu_core[ws_id_]);
  printf(
    "[Workspace %u] " 
    "Apply mbuf stalls: %lu, "
    "dispatcher mbuf usage: %.2f, "
    "mbuf reuse interval: %lf, "
    "App tx drop: %lu, "
    "Disp rx drop: %lu"
    "App rx avg num: %.2f\n",
    ws_id_, 
    stats_->app_apply_mbuf_stalls,
    (double)stats_->mbuf_usage/stats_->mbuf_alloc_times/Dispatcher::kSizeMemPool,
    stats_->app_tx_mbuf_trace_addr == nullptr
      ? (double)(stats_->app_tx_mbuf_reuse_interval) / (double)(stats_->app_tx_nb_traced_mbuf)
      : (double)(stats_->app_tx_mbuf_reuse_interval) / (double)(stats_->app_tx_nb_traced_mbuf - 1),
    stats_->app_enqueue_drops,
    stats_->disp_enqueue_drops,
    self_app_rx_batch
  );
  printf("[Workspace %u] TX Breakdown: throughput(App%.3f, Disp%.3f, NIC%.3f), latency(%.3f, %.3f, %.3f)\n", ws_id_, self_app_tx_tp, self_disp_tx_tp, self_nic_tx_tp, self_app_tx_compl + self_app_tx_stall, self_disp_tx_compl + self_disp_tx_stall, self_nic_tx_compl);
  printf("[Workspace %u] RX Breakdown: throughput(App%.3f, Disp%.3f, NIC%.3f), latency(%.3f, %.3f, %.3f)\n", ws_id_, self_app_rx_tp, self_disp_rx_tp, self_nic_rx_tp, self_app_rx_compl + self_app_rx_stall, self_disp_rx_compl + self_disp_rx_stall, self_nic_rx_compl);
  #ifdef OneStage
  printf("[Workspace %u] TX Single Stage Breakdown: throughput(App%.3f, Disp%.3f), latency(%.3f, %.3f), stall(%.3f, %.3f)\n", ws_id_, os_app_tx_tp, os_disp_tx_tp, self_app_tx_compl + self_app_tx_stall, self_disp_tx_compl + self_disp_tx_stall, self_app_tx_stall, self_disp_tx_stall);
  printf("[Workspace %u] RX Single Stage Breakdown: throughput(App%.3f, Disp%.3f), latency(%.3f, %.3f), stall(%.3f, %.3f)\n", ws_id_, os_app_rx_tp, os_disp_rx_tp, self_app_rx_compl + self_app_rx_stall, self_disp_rx_compl + self_disp_rx_stall, self_app_rx_stall, self_disp_rx_stall);
  #endif

  if(likely(stats_->mbuf_alloc_times > 0)){
    g_stats->disp_mbuf_usage += (double)(stats_->mbuf_usage) / (double)(stats_->mbuf_alloc_times) / (double)(Dispatcher::kSizeMemPool);
    // printf("mbuf_usage: %lu, mbuf_alloc_times: %u, mempool size: %lu, usage: %lf\n", stats_->mbuf_usage, stats_->mbuf_alloc_times, Dispatcher::kSizeMemPool, g_stats->disp_mbuf_usage);
  } else {
    g_stats->disp_mbuf_usage += 0.0f;
  }
}

template <class TDispatcher>
void Workspace<TDispatcher>::update_stats(uint8_t duration) {
  std::lock_guard<std::mutex> lock(context_->mutex_);
  context_->completed_ws_num_++;
  // printf("[Workspace %u] Completed\n", ws_id_);
  if (!context_->end_signal_) {
    context_->end_signal_ = true;
    /// The first ws will collect all ws stats
    uint8_t worker_num = 0, dispatcher_num = 0;
    std::vector<double> ws_freq;
    for (auto &ws_id : context_->active_ws_id_) {
      double freq = context_->ws_[ws_id]->get_freq();
      context_->ws_[ws_id]->aggregate_stats(context_->perf_stats_, freq, duration);
      if (context_->ws_[ws_id]->get_ws_type() & WORKER) {
        worker_num++;
      }
      if (context_->ws_[ws_id]->get_ws_type() & DISPATCHER) {
        dispatcher_num++;
      }
      ws_freq.push_back(freq);
    }
    /// Print ws freq for debug
    printf("Workspace freqs: ");
    for (auto &freq : ws_freq) {
      printf("%.2f ", freq);
    }
    printf("\n");
    /// Update latency
    context_->perf_stats_->app_tx_compl_ /= worker_num;
    context_->perf_stats_->app_tx_compl_avg_ /= worker_num;
    context_->perf_stats_->app_tx_stall_ /= worker_num;
    context_->perf_stats_->app_tx_stall_avg_ /= worker_num;
    context_->perf_stats_->app_rx_compl_ /= worker_num;
    context_->perf_stats_->app_rx_compl_avg_ /= worker_num;
    context_->perf_stats_->app_rx_stall_ /= worker_num;
    context_->perf_stats_->app_rx_stall_avg_ /= worker_num;

    context_->perf_stats_->disp_tx_compl_ /= dispatcher_num;
    context_->perf_stats_->disp_tx_stall_ /= dispatcher_num;
    context_->perf_stats_->disp_rx_compl_ /= dispatcher_num;
    context_->perf_stats_->disp_rx_stall_ /= dispatcher_num;

    context_->perf_stats_->nic_tx_compl_ /= dispatcher_num;
    context_->perf_stats_->nic_rx_compl_ /= dispatcher_num;

    context_->perf_stats_->disp_mbuf_usage /= dispatcher_num;

    stats_init_ws_ = true;
  }
}

template <class TDispatcher>
void Workspace<TDispatcher>::run_event_loop_timeout_st(uint8_t iteration, uint8_t seconds) {
  size_t core_idx = get_global_index(numa_node_, ws_id_);
  /// Warmup CPU
  set_cpu_freq_max(core_idx);
  /// Sync and print stats for each one second
  for (size_t i = 0; i < iteration; i++) {
    /// Loop init
    net_stats_init(stats_);
    nic_rx_prev_desc_ = 0;
    freq_ghz_ = measure_rdtsc_freq();
    // printf("Ws %u: Current CPU freq is %.2f\n", ws_id_, freq);
    size_t timeout_tsc = ms_to_cycles(1000*seconds, freq_ghz_);
    size_t interval_tsc = us_to_cycles(1.0, freq_ghz_);  // launch an event loop once per one us
    wait();

    /* Start loop */
    /// random start
    size_t wait_tsc = rdtsc(), random_tsc = context_->dis_(context_->gen_);
    while (rdtsc() - wait_tsc < random_tsc) {
      launch();
    }
    
    // printf("[Workspace %u] Start event loop, waiting for %lu\n", ws_id_, random_tsc);
    size_t start_tsc = rdtsc();
    size_t loop_tsc = start_tsc;
    nic_rx_prev_tick_ = start_tsc;
    while (true) {
      if (rdtsc() - loop_tsc > interval_tsc) {
        loop_tsc = rdtsc();
        launch();
      }
      if (unlikely(rdtsc() - start_tsc > timeout_tsc)) {
        /// Only the first workspace records the stats
        update_stats(seconds);
        break;
      }
    }
    /* Loop End */
    /// continue loop until all workspaces are completed
    while ((ws_type_ & DISPATCHER) && context_->completed_ws_num_ != context_->active_ws_id_.size()) {
      // printf("[Workspace %u] Waiting for other workspaces to complete, %u, %lu\n", ws_id_, context_->completed_ws_num_, context_->active_ws_id_.size());
      launch();
      /// waiting for 100ms
      wait_tsc = rdtsc();
      while (rdtsc() - wait_tsc < ms_to_cycles(100, freq_ghz_)) {
        continue;
      }
    }
    wait();
    /// Print and reset stats
    if (stats_init_ws_) {
      context_->perf_stats_->print_perf_stats(seconds);
      context_->init_perf_stats();
      context_->end_signal_ = false;
      context_->completed_ws_num_ = 0;
      stats_init_ws_ = false;
    }
  }
  set_cpu_freq_normal(core_idx);
}

FORCE_COMPILE_DISPATCHER
}  // namespace dperf