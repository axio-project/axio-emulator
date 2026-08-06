/**
 * @file workspace.cc
 * @brief Executing workspace of a datapath pipeline. In theory, workspace can
 * be called by various thread library such as pthread, datapath OS, DOCA, etc.
 */
#include "workspace.h"

namespace axio {

template <class TDispatcher>
Workspace<TDispatcher>::Workspace(WsContext *context, uint8_t ws_id, uint8_t ws_type, 
                                  uint8_t numa_node, uint8_t phy_port, 
                                  std::vector<axio::phase_t> *ws_loop,
                                  UserConfig *user_config)
    : context_(context),
      ws_id_(ws_id),
      ws_type_(ws_type),
      numa_node_(numa_node),
      phy_port_(phy_port),
      ws_loop_(ws_loop) {

  if (this->ws_type_ == 0) {
    AXIO_INFO("Workspace %u is not used\n", this->ws_id_);
    return;
  }

  // Parameter Check
  rt_assert(this->ws_type_ != kInvaildWorkspaceType, "Invalid workspace type");
  rt_assert(phy_port < kMaxPhyPorts, "Invalid physical port");
  rt_assert(this->numa_node_ < kMaxNumaNodes, "Invalid NUMA node");

  // Init and check tunable parameters
  const auto& tunables = user_config->tunables();
  rt_assert(tunables.app_core_count_ <= kWorkspaceMaxNum, "App core number is too large");
  this->app_tx_message_batch_size_ = tunables.app_tx_message_batch_size_;
  rt_assert(this->app_tx_message_batch_size_ <= kMaxBatchSize, "App TX batch size is too large");
  this->app_rx_message_batch_size_ = tunables.app_rx_message_batch_size_;
  rt_assert(this->app_rx_message_batch_size_ <= kMaxBatchSize, "App RX batch size is too large");

  // Check batch size to avoid deadlock
  rt_assert(kInflightMessageBudget >= this->app_tx_message_batch_size_, "kInflightMessageBudget is too small");
  rt_assert(kInflightMessageBudget >= this->app_rx_message_batch_size_, "kInflightMessageBudget is too small");

  // Check queue capacity is enough
  rt_assert(kWsQueueSize >= this->app_tx_message_batch_size_, "Application TX queue size is too small");
  rt_assert(kWsQueueSize >= this->app_rx_message_batch_size_, "Application RX queue size is too small");

  // Check memory pool size is enough
  rt_assert(Dispatcher::kMemPoolSize >= this->app_tx_message_batch_size_ * kAppRequestPktsNum, "Mempool size is too small");
  rt_assert(Dispatcher::kMemPoolSize >= this->app_rx_message_batch_size_ * kAppResponsePktsNum, "Mempool size is too small");

  /* Init workspace, phase 1 */
  if (this->ws_type_ & WORKER) {
    const auto& workloads = user_config->workloads();
    this->workload_type_ = workloads.workspace_workloads_.at(this->ws_id_);
    uint8_t group_idx = workloads.workspace_group_indices_.at(this->ws_id_);
    this->dispatcher_ws_id_ = workloads.dispatchers_.at(this->workload_type_).at(group_idx);
    /// config tx rule table
    for (auto remote_dispatcher_ws_id : workloads.remote_dispatchers_.at(this->workload_type_)) {
      this->tx_rule_table_->add_route(this->workload_type_, remote_dispatcher_ws_id);
    }
    printf("Workspace %u is assigned to workload %u, dispatcher %u\n", this->ws_id_, this->workload_type_, this->dispatcher_ws_id_);

    if constexpr (kMemoryAccessRangePerPkt > 0) {
      this->stateful_memory_ = malloc(kStatefulMemorySizePerCore);
      assert(this->stateful_memory_ != nullptr);
      memset(this->stateful_memory_, 'a', kStatefulMemorySizePerCore);
      this->stateful_memory_index_ = 0;
    }

    if (kRxMsgHandler == kRxMsgHandler_KV && NODE_TYPE == SERVER) {
      size_t initial_map_size = 10000;
      this->kv_store_ = new KV(initial_map_size);
    }
  }
  if (this->ws_type_ & DISPATCHER) {
    this->dispatcher_ = new TDispatcher(this->ws_id_, this->phy_port_, this->numa_node_, user_config);
  }
  // Register this workspace to ws context. Then, workspace can communicate with
  // each other through ws context.
  this->_register();

  // Wait for all workspaces to be registered
  this->_wait();

  /* Init workspace, phase 2 */
  if (this->ws_type_ & WORKER) {
    this->_set_memory_region();
    if (this->mem_reg_ == nullptr) {
      AXIO_ERROR("Workspace %u cannot get mem_reg\n", this->ws_id_);
      return;
    }
  }
  if (this->ws_type_ & DISPATCHER) {
    /// config rx rule table and workspace queues
    this->_configure_dispatcher();
    if (this->dispatcher_->get_ws_tx_queue_size() == 0) {
      AXIO_ERROR("Failed to config dispatcher %u\n", this->ws_id_);
      return;
    }
  }
  this->_wait();   // Force sync before launch
}

template <class TDispatcher>
Workspace<TDispatcher>::~Workspace(){
  AXIO_INFO("Destroying Ws %u.\n", this->ws_id_);
  delete this->dispatcher_;
}

template <class TDispatcher>
void Workspace<TDispatcher>::_register() {
  std::lock_guard<std::mutex> lock(this->context_->mutex_);
  rt_assert(this->context_->workspaces_[this->ws_id_] == nullptr, "Workspace already registered!");
  this->context_->workspaces_[this->ws_id_] = this;
  this->context_->active_workspace_ids_.push_back(this->ws_id_);
  if (this->ws_type_ & WORKER) {
    this->context_->workspace_tx_queues_[this->ws_id_] = this->tx_queue_;
    this->context_->workspace_rx_queues_[this->ws_id_] = this->rx_queue_;
    this->context_->workspace_dispatchers_[this->ws_id_] = this->dispatcher_ws_id_;
  }
  if (this->ws_type_ & DISPATCHER) {
    if (this->context_->memory_regions_.find(this->ws_id_) != this->context_->memory_regions_.end()) {
      AXIO_ERROR("Dispatcher %u already registered\n", this->ws_id_);
      return;
    }
    this->context_->memory_regions_.insert(std::make_pair(this->ws_id_, this->dispatcher_->get_mem_reg()));
  }
}

template <class TDispatcher>
void Workspace<TDispatcher>::_set_memory_region() {
  std::lock_guard<std::mutex> lock(this->context_->mutex_);
  this->mem_reg_ = this->context_->memory_regions_[this->dispatcher_ws_id_];
}

template <class TDispatcher>
void Workspace<TDispatcher>::_configure_dispatcher() {
  std::lock_guard<std::mutex> lock(this->context_->mutex_);
  for (auto &ws_id : this->context_->active_workspace_ids_) {
    auto it = this->context_->workspace_dispatchers_.find(ws_id);
    if (it != this->context_->workspace_dispatchers_.end() && it->second == this->ws_id_) {
      /// get one worker assigned to this dispatcher
      uint8_t workload_type = this->context_->workspaces_[ws_id]->_workload_type();
      this->dispatcher_->add_ws_tx_queue(this->context_->workspace_tx_queues_[ws_id]);
      this->dispatcher_->add_ws_rx_queue(ws_id, this->context_->workspace_rx_queues_[ws_id]);
      this->dispatcher_->add_rx_rule(workload_type, ws_id);
    }
  }
}

template <class TDispatcher>
void Workspace<TDispatcher>::launch() {
  for (auto &phase : *this->ws_loop_) {
    (this->*phase)();
  }
}

template <class TDispatcher>
void Workspace<TDispatcher>::_aggregate_stats(perf_stats *g_stats, double freq, uint8_t duration){
  /// App
  double self_app_tx_tp = 0, self_app_rx_tp = 0;
  double self_app_tx_compl = 0, self_app_tx_compl_avg = 0, self_app_tx_compl_min = 0, self_app_tx_compl_max = 0;
  double self_app_tx_stall = 0, self_app_tx_stall_avg = 0, self_app_tx_stall_min = 0, self_app_tx_stall_max = 0;
  double self_app_rx_compl = 0, self_app_rx_compl_avg = 0, self_app_rx_compl_min = 0, self_app_rx_compl_max = 0;
  double self_app_rx_stall = 0, self_app_rx_stall_avg = 0, self_app_rx_stall_min = 0, self_app_rx_stall_max = 0;
  double self_app_rx_batch = 0;

  self_app_tx_tp = (double)this->stats_->app_tx_msg_num / 1e6 / duration;
  self_app_rx_tp = (double)this->stats_->app_rx_msg_num / 1e6 / duration;

  if (this->stats_->app_tx_msg_num) {
    // app tx phrase 2
    self_app_tx_compl = to_usec(this->stats_->app_tx_avg_duration, freq) / this->stats_->app_tx_msg_num;
    self_app_tx_compl_avg = to_usec(this->stats_->app_tx_avg_duration, freq) / this->stats_->app_tx_invoke_times;
    self_app_tx_compl_min = to_usec(this->stats_->app_tx_min_duration, freq);
    self_app_tx_compl_max = to_usec(this->stats_->app_tx_max_duration, freq);

    g_stats->app_tx_compl_ += self_app_tx_compl;
    g_stats->app_tx_compl_max_  = g_stats->app_tx_compl_max_ < self_app_tx_compl_max 
                                ? self_app_tx_compl_max : g_stats->app_tx_compl_max_;
    g_stats->app_tx_compl_min_  = g_stats->app_tx_compl_min_ > self_app_tx_compl_min
                                ? self_app_tx_compl_min : g_stats->app_tx_compl_min_;
    g_stats->app_tx_compl_avg_ += self_app_tx_compl_avg;

    // app tx phrase 1
    self_app_tx_stall = to_usec(this->stats_->app_tx_stall_avg_duration, freq) / this->stats_->app_tx_msg_num;
    self_app_tx_stall_avg = to_usec(this->stats_->app_tx_stall_avg_duration, freq) / this->stats_->app_tx_invoke_times;
    self_app_tx_stall_min = to_usec(this->stats_->app_tx_stall_min_duration, freq);
    self_app_tx_stall_max = to_usec(this->stats_->app_tx_stall_max_duration, freq);
    
    g_stats->app_tx_stall_ += self_app_tx_stall;
    g_stats->app_tx_stall_max_ = g_stats->app_tx_stall_max_ < self_app_tx_stall_max 
                                ? self_app_tx_stall_max : g_stats->app_tx_stall_max_;
    g_stats->app_tx_stall_min_ = g_stats->app_tx_stall_min_ > self_app_tx_stall_min
                                ? self_app_tx_stall_min : g_stats->app_tx_stall_min_;
    g_stats->app_tx_stall_avg_ += self_app_tx_stall_avg;
  }
  if (this->stats_->app_rx_msg_num ) {
    self_app_rx_batch = (double)this->stats_->app_rx_msg_num / this->stats_->app_rx_invoke_times;
    // app rx phrase 2
    self_app_rx_compl = to_usec(this->stats_->app_rx_avg_duration, freq) / this->stats_->app_rx_msg_num;
    self_app_rx_compl_avg = to_usec(this->stats_->app_rx_avg_duration, freq) / this->stats_->app_rx_invoke_times;
    self_app_rx_compl_min = to_usec(this->stats_->app_rx_min_duration, freq);
    self_app_rx_compl_max = to_usec(this->stats_->app_rx_max_duration, freq);

    g_stats->app_rx_compl_ += self_app_rx_compl;
    g_stats->app_rx_compl_max_  = g_stats->app_rx_compl_max_ < self_app_rx_compl_max 
                                ? self_app_rx_compl_max : g_stats->app_rx_compl_max_;
    g_stats->app_rx_compl_min_  = g_stats->app_rx_compl_min_ > self_app_rx_compl_min
                                ? self_app_rx_compl_min : g_stats->app_rx_compl_min_;
    g_stats->app_rx_compl_avg_ += self_app_rx_compl_avg;

    // app rx phrase 1
    self_app_rx_stall = to_usec(this->stats_->app_rx_stall_avg_duration, freq) / this->stats_->app_rx_msg_num;
    self_app_rx_stall_avg = to_usec(this->stats_->app_rx_stall_avg_duration, freq) / this->stats_->app_rx_invoke_times;
    self_app_rx_stall_min = to_usec(this->stats_->app_rx_stall_min_duration, freq);
    self_app_rx_stall_max = to_usec(this->stats_->app_rx_stall_max_duration, freq);
    
    g_stats->app_rx_stall_ += self_app_rx_stall;
    g_stats->app_rx_stall_max_ = g_stats->app_rx_stall_max_ < self_app_rx_stall_max 
                                ? self_app_rx_stall_max : g_stats->app_rx_stall_max_;
    g_stats->app_rx_stall_min_ = g_stats->app_rx_stall_min_ > self_app_rx_stall_min
                                ? self_app_rx_stall_min : g_stats->app_rx_stall_min_;
    g_stats->app_rx_stall_avg_ += self_app_rx_stall_avg;
  }

  /// Dispatcher
  double self_disp_tx_tp = 0, self_disp_rx_tp = 0, self_disp_tx_compl = 0, self_disp_tx_stall = 0, self_disp_rx_compl = 0, self_disp_rx_stall = 0;
  self_disp_tx_tp = (double)this->stats_->disp_tx_pkt_num / 1e6 / duration;
  self_disp_rx_tp = (double)this->stats_->disp_rx_pkt_num / 1e6 / duration;
  if (this->stats_->disp_tx_pkt_num) {
    self_disp_tx_compl = to_usec(this->stats_->disp_tx_duration, freq) / this->stats_->disp_tx_pkt_num;
    g_stats->disp_tx_compl_ += self_disp_tx_compl;
    self_disp_tx_stall = to_usec(this->stats_->disp_tx_stall_duration, freq) / this->stats_->disp_tx_pkt_num;
    g_stats->disp_tx_stall_ += self_disp_tx_stall;
  }
  if (this->stats_->disp_rx_pkt_num) {
    self_disp_rx_compl = to_usec(this->stats_->disp_rx_duration, freq) / this->stats_->disp_rx_pkt_num;
    g_stats->disp_rx_compl_ += self_disp_rx_compl;
    self_disp_rx_stall = to_usec(this->stats_->disp_rx_stall_duration, freq) / this->stats_->disp_rx_pkt_num;
    g_stats->disp_rx_stall_ += self_disp_rx_stall;
  }

  /// NIC
  double self_nic_tx_tp = 0, self_nic_rx_tp = 0, self_nic_tx_compl = 0, self_nic_rx_compl = 0;
  /// nic tx is same with disp tx stall
  self_nic_tx_tp = (double)this->stats_->nic_tx_pkt_num / 1e6 / duration;
  if (this->stats_->nic_tx_pkt_num) {
    self_nic_tx_compl = to_usec(this->stats_->disp_tx_stall_duration, freq) / this->stats_->nic_tx_pkt_num;
    g_stats->nic_tx_compl_ += self_nic_tx_compl;
  }
  if (this->stats_->nic_rx_times) {
    self_nic_rx_compl = to_usec(static_cast<size_t>(std::round(this->stats_->nic_rx_cpt)), freq) / this->stats_->nic_rx_times;
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
  // printf("[Workspace %u] bind to core %lu\n", this->ws_id_, this->context_->cpu_cores_[this->ws_id_]);
  printf(
    "[Workspace %u] " 
    "Apply mbuf stalls: %lu, "
    "dispatcher mbuf usage: %.2f, "
    "mbuf reuse interval: %lf, "
    "App tx drop: %lu, "
    "Disp rx drop: %lu "
    "App rx avg num: %.2f\n",
    this->ws_id_,
    this->stats_->app_apply_mbuf_stalls,
    (double)this->stats_->mbuf_usage/this->stats_->mbuf_alloc_times/Dispatcher::kMemPoolSize,
    this->stats_->app_tx_mbuf_trace_addr == nullptr
      ? (double)(this->stats_->app_tx_mbuf_reuse_interval) / (double)(this->stats_->app_tx_nb_traced_mbuf)
      : (double)(this->stats_->app_tx_mbuf_reuse_interval) / (double)(this->stats_->app_tx_nb_traced_mbuf - 1),
    this->stats_->app_enqueue_drops,
    this->stats_->disp_enqueue_drops,
    self_app_rx_batch
  );
  printf("[Workspace %u] TX Breakdown: throughput(App%.3f, Disp%.3f, NIC%.3f), latency(%.3f, %.3f, %.3f)\n", this->ws_id_, self_app_tx_tp, self_disp_tx_tp, self_nic_tx_tp, self_app_tx_compl + self_app_tx_stall, self_disp_tx_compl + self_disp_tx_stall, self_nic_tx_compl);
  printf("[Workspace %u] RX Breakdown: throughput(App%.3f, Disp%.3f, NIC%.3f), latency(%.3f, %.3f, %.3f)\n", this->ws_id_, self_app_rx_tp, self_disp_rx_tp, self_nic_rx_tp, self_app_rx_compl + self_app_rx_stall, self_disp_rx_compl + self_disp_rx_stall, self_nic_rx_compl);
  #ifdef OneStage
  printf("[Workspace %u] TX Single Stage Breakdown: throughput(App%.3f, Disp%.3f), latency(%.3f, %.3f), stall(%.3f, %.3f)\n", this->ws_id_, os_app_tx_tp, os_disp_tx_tp, self_app_tx_compl + self_app_tx_stall, self_disp_tx_compl + self_disp_tx_stall, self_app_tx_stall, self_disp_tx_stall);
  printf("[Workspace %u] RX Single Stage Breakdown: throughput(App%.3f, Disp%.3f), latency(%.3f, %.3f), stall(%.3f, %.3f)\n", this->ws_id_, os_app_rx_tp, os_disp_rx_tp, self_app_rx_compl + self_app_rx_stall, self_disp_rx_compl + self_disp_rx_stall, self_app_rx_stall, self_disp_rx_stall);
  #endif

  if(likely(this->stats_->mbuf_alloc_times > 0)){
    g_stats->disp_mbuf_usage += (double)(this->stats_->mbuf_usage) / (double)(this->stats_->mbuf_alloc_times) / (double)(Dispatcher::kMemPoolSize);
    // printf("mbuf_usage: %lu, mbuf_alloc_times: %u, mempool size: %lu, usage: %lf\n", this->stats_->mbuf_usage, this->stats_->mbuf_alloc_times, Dispatcher::kMemPoolSize, g_stats->disp_mbuf_usage);
  } else {
    g_stats->disp_mbuf_usage += 0.0f;
  }
}

template <class TDispatcher>
void Workspace<TDispatcher>::_update_stats(uint8_t duration) {
  std::lock_guard<std::mutex> lock(this->context_->mutex_);
  this->context_->completed_workspace_count_++;
  // printf("[Workspace %u] Completed\n", this->ws_id_);
  if (!this->context_->end_signal_) {
    this->context_->end_signal_ = true;
    /// The first ws will collect all ws stats
    uint8_t worker_num = 0, dispatcher_num = 0;
    std::vector<double> ws_freq;
    for (auto &ws_id : this->context_->active_workspace_ids_) {
      double freq = this->context_->workspaces_[ws_id]->_frequency_ghz();
      this->context_->workspaces_[ws_id]->_aggregate_stats(
          &this->context_->performance_stats_, freq, duration);
      if (this->context_->workspaces_[ws_id]->_type() & WORKER) {
        worker_num++;
      }
      if (this->context_->workspaces_[ws_id]->_type() & DISPATCHER) {
        dispatcher_num++;
      }
      ws_freq.push_back(freq);
    }
    /// Print ws freq for debug
    double avg_freq = 0;
    printf("Workspace freqs: ");
    for (auto &freq : ws_freq) {
      printf("%.2f ", freq);
      avg_freq += freq;
    }
    printf("\n");
    avg_freq /= ws_freq.size();
    /// Update latency
    this->context_->performance_stats_.app_tx_compl_ /= worker_num;
    this->context_->performance_stats_.app_tx_compl_avg_ /= worker_num;
    this->context_->performance_stats_.app_tx_stall_ /= worker_num;
    this->context_->performance_stats_.app_tx_stall_avg_ /= worker_num;
    this->context_->performance_stats_.app_rx_compl_ /= worker_num;
    this->context_->performance_stats_.app_rx_compl_avg_ /= worker_num;
    this->context_->performance_stats_.app_rx_stall_ /= worker_num;
    this->context_->performance_stats_.app_rx_stall_avg_ /= worker_num;

    this->context_->performance_stats_.disp_tx_compl_ /= dispatcher_num;
    this->context_->performance_stats_.disp_tx_stall_ /= dispatcher_num;
    this->context_->performance_stats_.disp_rx_compl_ /= dispatcher_num;
    this->context_->performance_stats_.disp_rx_stall_ /= dispatcher_num;

    this->context_->performance_stats_.nic_tx_compl_ /= dispatcher_num;
    this->context_->performance_stats_.nic_rx_compl_ /= dispatcher_num;

    this->context_->performance_stats_.disp_mbuf_usage /= dispatcher_num;

    /// calculate P50, P99, P99.9 latency
    /// sort this->latency_samples_
  #if PERF_TEST_LAT == 1 && NODE_TYPE == CLIENT
    std::sort(this->latency_samples_, this->latency_samples_ + PERF_LAT_SAMPLE_NUM);
    size_t p50_idx = PERF_LAT_SAMPLE_NUM / 2;
    size_t p99_idx = PERF_LAT_SAMPLE_NUM * 99 / 100;
    size_t p999_idx = PERF_LAT_SAMPLE_NUM * 999 / 1000;
    printf("P50: %.2f, P99: %.2f, P99.9: %.2f\n", to_usec(this->latency_samples_[p50_idx], avg_freq), to_usec(this->latency_samples_[p99_idx], avg_freq), to_usec(this->latency_samples_[p999_idx], avg_freq));
  #endif
    this->stats_init_ws_ = true;
  }
}

template <class TDispatcher>
void Workspace<TDispatcher>::run_event_loop_timeout_st(uint8_t iteration, uint8_t seconds) {
  size_t core_idx = get_global_index(this->numa_node_, this->ws_id_);
  /// Warmup CPU
  set_cpu_freq_max(core_idx);
  /// Sync and print stats for each one second
  for (size_t i = 0; i < iteration; i++) {
    /// Loop init
    net_stats_init(this->stats_);
    this->nic_rx_prev_desc_ = 0;
    this->freq_ghz_ = measure_rdtsc_freq();
    // printf("Ws %u: Current CPU freq is %.2f\n", this->ws_id_, freq);
    size_t timeout_tsc = ms_to_cycles(1000*seconds, this->freq_ghz_);
    size_t interval_tsc = us_to_cycles(1.0, this->freq_ghz_);  // launch an event loop once per one us
    this->_wait();

    /* Start loop */
    /// random start
    size_t wait_tsc = rdtsc(), random_tsc = this->context_->random_distribution_(this->context_->random_generator_);
    while (rdtsc() - wait_tsc < random_tsc) {
      launch();
    }
    
    // printf("[Workspace %u] Start event loop, waiting for %lu\n", this->ws_id_, random_tsc);
    size_t start_tsc = rdtsc();
    size_t loop_tsc = start_tsc;
    size_t lat_start_tick = start_tsc;
    size_t lat_sended_pkt_num = 0;
    this->nic_rx_prev_tick_ = start_tsc;
    while (true) {
      if (rdtsc() - loop_tsc > interval_tsc) {
        loop_tsc = rdtsc();
        launch();
        /// latency stats
      #if PERF_TEST_LAT == 1 && NODE_TYPE == CLIENT
        if (unlikely(lat_sended_pkt_num < this->stats_->app_rx_msg_num)) {
          // 使用单次rdtscp调用优化
          size_t end_tick = dpath_rdtsc();
          this->latency_samples_[this->latency_sample_index_] = end_tick - lat_start_tick;
          this->latency_sample_index_ = (this->latency_sample_index_ + 1) % PERF_LAT_SAMPLE_NUM;
          lat_start_tick = end_tick;  // 重用时间戳，减少一次rdtsc调用
          lat_sended_pkt_num = this->stats_->app_tx_msg_num;
        }
      #endif
      }
      if (unlikely(rdtsc() - start_tsc > timeout_tsc)) {
        /// Only the first workspace records the stats
        this->_update_stats(seconds);
        break;
      }
    }
    /* Loop End */
    /// continue loop until all workspaces are completed
    while ((this->ws_type_ & DISPATCHER) && this->context_->completed_workspace_count_ != this->context_->active_workspace_ids_.size()) {
      // printf("[Workspace %u] Waiting for other workspaces to complete, %u, %lu\n", this->ws_id_, this->context_->completed_workspace_count_, this->context_->active_workspace_ids_.size());
      launch();
      /// waiting for 100ms
      wait_tsc = rdtsc();
      while (rdtsc() - wait_tsc < ms_to_cycles(100, this->freq_ghz_)) {
        continue;
      }
    }
    this->_wait();
    /// Print and reset stats
    if (this->stats_init_ws_) {
      this->context_->performance_stats_.print_perf_stats(seconds);
      this->context_->_initialize_performance_stats();
      this->context_->end_signal_ = false;
      this->context_->completed_workspace_count_ = 0;
      this->stats_init_ws_ = false;
    }
  }
  set_cpu_freq_normal(core_idx);
}

AXIO_FORCE_COMPILE_DISPATCHER
}  // namespace axio
