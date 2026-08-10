/**
 * @file workspace.cc
 * @brief Executing workspace of a datapath pipeline. In theory, workspace can
 * be called by various thread library such as pthread, datapath OS, DOCA, etc.
 */
#include "workspace.h"

namespace axio {

template <class TDispatcher>
Workspace<TDispatcher>::Workspace(WsContext* context, uint8_t ws_id,
                                  uint8_t ws_type, uint8_t numa_node,
                                  uint8_t phy_port,
                                  std::vector<WorkspacePhase>* workspace_loop,
                                  UserConfig* user_config)
    : context_(context),
      ws_id_(ws_id),
      ws_type_(ws_type),
      numa_node_(numa_node),
      phy_port_(phy_port),
      ws_loop_(workspace_loop),
      metrics_enabled_(context->metrics_enabled_) {

  if (this->ws_type_ == 0) {
    AXIO_INFO("Workspace %u is not used\n", this->ws_id_);
    return;
  }

  // Parameter Check
  rt_assert(this->ws_type_ != kInvalidWorkspaceType, "Invalid workspace type");
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
#if AXIO_ENABLE_INFLIGHT_LIMIT
  rt_assert(kInflightMessageBudget >= this->app_tx_message_batch_size_, "kInflightMessageBudget is too small");
  rt_assert(kInflightMessageBudget >= this->app_rx_message_batch_size_, "kInflightMessageBudget is too small");
#endif

  // Check queue capacity is enough
  rt_assert(kWsQueueSize >= this->app_tx_message_batch_size_, "Application TX queue size is too small");
  rt_assert(kWsQueueSize >= this->app_rx_message_batch_size_, "Application RX queue size is too small");

  // Check memory pool size is enough
  rt_assert(Dispatcher::kMemPoolSize >= this->app_tx_message_batch_size_ * kAppRequestPktsNum, "Mempool size is too small");
  rt_assert(Dispatcher::kMemPoolSize >= this->app_rx_message_batch_size_ * kAppResponsePktsNum, "Mempool size is too small");

  /* Init workspace, phase 1 */
  const auto& workloads = user_config->workloads();
  if (this->ws_type_ & kApplicationWorkspace) {
    this->workload_type_ = workloads.workspace_workloads_.at(this->ws_id_);
    uint8_t group_idx = workloads.workspace_group_indices_.at(this->ws_id_);
    this->dispatcher_ws_id_ = workloads.dispatchers_.at(this->workload_type_).at(group_idx);
    /// config tx rule table
    for (auto remote_dispatcher_ws_id : workloads.remote_dispatchers_.at(this->workload_type_)) {
      this->tx_rule_table_->add_route(this->workload_type_, remote_dispatcher_ws_id);
    }
    printf("Workspace %u is assigned to workload %u, dispatcher %u\n", this->ws_id_, this->workload_type_, this->dispatcher_ws_id_);

    if constexpr (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerMemory) {
      this->stateful_memory_ = malloc(kMAppStateBytes);
      assert(this->stateful_memory_ != nullptr);
      memset(this->stateful_memory_, 'a', kMAppStateBytes);
      this->stateful_memory_index_ = 0;
      this->memory_workload_ = new workloads::MemoryWorkload(
          kMAppStateBytes, kMAppAccessBytesPerMessage,
          kMAppRandomSeed + this->ws_id_);
    } else if constexpr (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerFileWrite ||
                         AXIO_RX_MESSAGE_HANDLER == kMessageHandlerFileRead) {
      this->stateful_memory_ = malloc(kFileStatefulMemorySizePerCore);
      assert(this->stateful_memory_ != nullptr);
      memset(this->stateful_memory_, 'a', kFileStatefulMemorySizePerCore);
      this->stateful_memory_index_ = 0;
    }

    if constexpr (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerKeyValue) {
      if constexpr (AXIO_NODE_TYPE == AXIO_SERVER) {
        size_t application_index = 0;
        size_t current_index = 0;
        for (const config::WorkspaceId active_id :
             user_config->topology().active_workspace_ids()) {
          if (!config::has_role(
                  user_config->topology().roles(active_id),
                  config::WorkspaceRole::kApplication)) {
            continue;
          }
          if (active_id.value() == this->ws_id_) {
            application_index = current_index;
            break;
          }
          ++current_index;
        }
        const workloads::KeyValueShard shard = workloads::key_value_shard(
            kKeyValueEntryCount,
            user_config->topology().application_core_count(),
            application_index);
        this->key_value_store_ = new KeyValueStore(
            shard.size, shard.first,
            kKeyValueRandomSeed + application_index);
      } else {
        this->key_value_operation_mix_ =
            new workloads::DeterministicOperationMix(kKeyValueGetRatio);
      }
    }
  }
  if (this->ws_type_ & kDispatcherWorkspace) {
    for (const auto& workload_dispatchers : workloads.dispatchers_) {
      if (std::find(workload_dispatchers.second.begin(),
                    workload_dispatchers.second.end(), this->ws_id_) !=
          workload_dispatchers.second.end()) {
        this->dispatcher_workload_ids_.push_back(workload_dispatchers.first);
      }
    }
    this->dispatcher_ = new TDispatcher(this->ws_id_, this->phy_port_, this->numa_node_, user_config);
  }
  // Register this workspace to ws context. Then, workspace can communicate with
  // each other through ws context.
  this->_register();

  // Wait for all workspaces to be registered
  this->_wait();

  /* Init workspace, phase 2 */
  if (this->ws_type_ & kApplicationWorkspace) {
    this->_set_memory_region();
    if (this->mem_reg_ == nullptr) {
      AXIO_ERROR("Workspace %u cannot get mem_reg\n", this->ws_id_);
      return;
    }
  }
  if (this->ws_type_ & kDispatcherWorkspace) {
    /// config rx rule table and workspace queues
    this->_configure_dispatcher();
    if (this->dispatcher_->workspace_tx_queue_count() == 0) {
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
  delete this->memory_workload_;
  delete this->key_value_store_;
  delete this->key_value_operation_mix_;
  free(this->stateful_memory_);
}

template <class TDispatcher>
void Workspace<TDispatcher>::_register() {
  std::lock_guard<std::mutex> lock(this->context_->mutex_);
  rt_assert(this->context_->workspaces_[this->ws_id_] == nullptr, "Workspace already registered!");
  this->context_->workspaces_[this->ws_id_] = this;
  const auto insertion_point =
      std::lower_bound(this->context_->active_workspace_ids_.begin(),
                       this->context_->active_workspace_ids_.end(),
                       this->ws_id_);
  this->context_->active_workspace_ids_.insert(insertion_point, this->ws_id_);
  if (this->ws_type_ & kApplicationWorkspace) {
    this->context_->workspace_tx_queues_[this->ws_id_] = this->tx_queue_;
    this->context_->workspace_rx_queues_[this->ws_id_] = this->rx_queue_;
    this->context_->workspace_dispatchers_[this->ws_id_] = this->dispatcher_ws_id_;
  }
  if (this->ws_type_ & kDispatcherWorkspace) {
    if (this->context_->memory_regions_.find(this->ws_id_) != this->context_->memory_regions_.end()) {
      AXIO_ERROR("Dispatcher %u already registered\n", this->ws_id_);
      return;
    }
    this->context_->memory_regions_.insert(std::make_pair(this->ws_id_, this->dispatcher_->memory_region()));
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
      this->dispatcher_->add_workspace_tx_queue(this->context_->workspace_tx_queues_[ws_id]);
      this->dispatcher_->add_workspace_rx_queue(ws_id, this->context_->workspace_rx_queues_[ws_id]);
      this->dispatcher_->add_rx_route(workload_type, ws_id);
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
void Workspace<TDispatcher>::_aggregate_stats(PerformanceStats* g_stats,
                                               double freq,
                                               uint8_t duration,
                                               std::vector<
                                                   metrics::QueueCompletionInterval>*
                                                   nic_rx_intervals,
                                               std::vector<
                                                   metrics::QueueMetricsRecord>*
                                                   nic_rx_queues) {
  /// App
  double self_app_tx_tp = 0, self_app_rx_tp = 0;
  double self_app_tx_compl = 0, self_app_tx_compl_avg = 0, self_app_tx_compl_min = 0, self_app_tx_compl_max = 0;
  double self_app_tx_stall = 0, self_app_tx_stall_avg = 0, self_app_tx_stall_min = 0, self_app_tx_stall_max = 0;
  double self_app_rx_compl = 0, self_app_rx_compl_avg = 0, self_app_rx_compl_min = 0, self_app_rx_compl_max = 0;
  double self_app_rx_stall = 0, self_app_rx_stall_avg = 0, self_app_rx_stall_min = 0, self_app_rx_stall_max = 0;

  self_app_tx_tp = (double)this->stats_->app_tx_message_count_ / 1e6 / duration;
  self_app_rx_tp = (double)this->stats_->app_rx_message_count_ / 1e6 / duration;

  if (this->stats_->app_tx_message_count_) {
    // app tx phrase 2
    self_app_tx_compl = to_usec(this->stats_->app_tx_total_duration_, freq) / this->stats_->app_tx_message_count_;
    self_app_tx_compl_avg = to_usec(this->stats_->app_tx_total_duration_, freq) / this->stats_->app_tx_invocation_count_;
    self_app_tx_compl_min = to_usec(this->stats_->app_tx_min_duration_, freq);
    self_app_tx_compl_max = to_usec(this->stats_->app_tx_max_duration_, freq);

    g_stats->app_tx_compl_ += self_app_tx_compl;
    g_stats->app_tx_compl_max_  = g_stats->app_tx_compl_max_ < self_app_tx_compl_max 
                                ? self_app_tx_compl_max : g_stats->app_tx_compl_max_;
    g_stats->app_tx_compl_min_  = g_stats->app_tx_compl_min_ > self_app_tx_compl_min
                                ? self_app_tx_compl_min : g_stats->app_tx_compl_min_;
    g_stats->app_tx_compl_avg_ += self_app_tx_compl_avg;

    // app tx phrase 1
    self_app_tx_stall = to_usec(this->stats_->app_tx_stall_total_duration_, freq) / this->stats_->app_tx_message_count_;
    self_app_tx_stall_avg = to_usec(this->stats_->app_tx_stall_total_duration_, freq) / this->stats_->app_tx_invocation_count_;
    self_app_tx_stall_min = to_usec(this->stats_->app_tx_stall_min_duration_, freq);
    self_app_tx_stall_max = to_usec(this->stats_->app_tx_stall_max_duration_, freq);
    
    g_stats->app_tx_stall_ += self_app_tx_stall;
    g_stats->app_tx_stall_max_ = g_stats->app_tx_stall_max_ < self_app_tx_stall_max 
                                ? self_app_tx_stall_max : g_stats->app_tx_stall_max_;
    g_stats->app_tx_stall_min_ = g_stats->app_tx_stall_min_ > self_app_tx_stall_min
                                ? self_app_tx_stall_min : g_stats->app_tx_stall_min_;
    g_stats->app_tx_stall_avg_ += self_app_tx_stall_avg;
  }
  if (this->stats_->app_rx_message_count_) {
    // app rx phrase 2
    self_app_rx_compl = to_usec(this->stats_->app_rx_total_duration_, freq) / this->stats_->app_rx_message_count_;
    self_app_rx_compl_avg = to_usec(this->stats_->app_rx_total_duration_, freq) / this->stats_->app_rx_invocation_count_;
    self_app_rx_compl_min = to_usec(this->stats_->app_rx_min_duration_, freq);
    self_app_rx_compl_max = to_usec(this->stats_->app_rx_max_duration_, freq);

    g_stats->app_rx_compl_ += self_app_rx_compl;
    g_stats->app_rx_compl_max_  = g_stats->app_rx_compl_max_ < self_app_rx_compl_max 
                                ? self_app_rx_compl_max : g_stats->app_rx_compl_max_;
    g_stats->app_rx_compl_min_  = g_stats->app_rx_compl_min_ > self_app_rx_compl_min
                                ? self_app_rx_compl_min : g_stats->app_rx_compl_min_;
    g_stats->app_rx_compl_avg_ += self_app_rx_compl_avg;

    // app rx phrase 1
    self_app_rx_stall = to_usec(this->stats_->app_rx_stall_total_duration_, freq) / this->stats_->app_rx_message_count_;
    self_app_rx_stall_avg = to_usec(this->stats_->app_rx_stall_total_duration_, freq) / this->stats_->app_rx_invocation_count_;
    self_app_rx_stall_min = to_usec(this->stats_->app_rx_stall_min_duration_, freq);
    self_app_rx_stall_max = to_usec(this->stats_->app_rx_stall_max_duration_, freq);
    
    g_stats->app_rx_stall_ += self_app_rx_stall;
    g_stats->app_rx_stall_max_ = g_stats->app_rx_stall_max_ < self_app_rx_stall_max 
                                ? self_app_rx_stall_max : g_stats->app_rx_stall_max_;
    g_stats->app_rx_stall_min_ = g_stats->app_rx_stall_min_ > self_app_rx_stall_min
                                ? self_app_rx_stall_min : g_stats->app_rx_stall_min_;
    g_stats->app_rx_stall_avg_ += self_app_rx_stall_avg;
  }

  /// Dispatcher
  double self_disp_tx_tp = 0, self_disp_rx_tp = 0, self_disp_tx_compl = 0, self_disp_tx_stall = 0, self_disp_rx_compl = 0, self_disp_rx_stall = 0;
  self_disp_tx_tp = (double)this->stats_->dispatcher_tx_packet_count_ / 1e6 / duration;
  self_disp_rx_tp = (double)this->stats_->dispatcher_rx_packet_count_ / 1e6 / duration;
  if (this->stats_->dispatcher_tx_packet_count_) {
    self_disp_tx_compl = to_usec(this->stats_->dispatcher_tx_duration_, freq) / this->stats_->dispatcher_tx_packet_count_;
    g_stats->disp_tx_compl_ += self_disp_tx_compl;
    self_disp_tx_stall = to_usec(this->stats_->dispatcher_tx_stall_duration_, freq) / this->stats_->dispatcher_tx_packet_count_;
    g_stats->disp_tx_stall_ += self_disp_tx_stall;
  }
  if (this->stats_->dispatcher_rx_packet_count_) {
    self_disp_rx_compl = to_usec(this->stats_->dispatcher_rx_duration_, freq) / this->stats_->dispatcher_rx_packet_count_;
    g_stats->disp_rx_compl_ += self_disp_rx_compl;
    self_disp_rx_stall = to_usec(this->stats_->dispatcher_rx_stall_duration_, freq) / this->stats_->dispatcher_rx_packet_count_;
    g_stats->disp_rx_stall_ += self_disp_rx_stall;
  }

  /// NIC
  double self_nic_tx_tp = 0, self_nic_rx_tp = 0, self_nic_tx_compl = 0;
#ifdef AXIO_ONE_STAGE
  double self_nic_rx_compl = 0;
#endif
  /// nic tx is same with disp tx stall
  self_nic_tx_tp = (double)this->stats_->nic_tx_packet_count_ / 1e6 / duration;
  if (this->stats_->nic_tx_packet_count_) {
    self_nic_tx_compl = to_usec(this->stats_->dispatcher_tx_stall_duration_, freq) / this->stats_->nic_tx_packet_count_;
    g_stats->nic_tx_compl_ += self_nic_tx_compl;
  }
  self_nic_rx_tp =
      static_cast<double>(this->stats_->nic_rx_packet_count_) / 1e6 /
      duration;
  if (this->ws_type_ & kDispatcherWorkspace) {
    const metrics::RxCompletionSnapshot nic_rx_snapshot =
        this->stats_->nic_rx_completion_window_.snapshot();
    metrics::QueueMetricsRecord queue;
    queue.workspace_id = this->ws_id_;
    queue.workload_ids = this->dispatcher_workload_ids_;
    queue.successful_completion_count =
        nic_rx_snapshot.total_completion_count;
    queue.timed_completion_count = nic_rx_snapshot.timed_completion_count;
    queue.successful_poll_count = nic_rx_snapshot.successful_poll_count;
    queue.empty_poll_count = nic_rx_snapshot.empty_poll_count;
    queue.completion_error_count = nic_rx_snapshot.completion_error_count;
    queue.first_completion_tsc = nic_rx_snapshot.first_completion_tsc;
    queue.last_completion_tsc = nic_rx_snapshot.last_completion_tsc;
    queue.tsc_frequency_ghz = freq;
    queue.clock_valid = nic_rx_snapshot.clock_valid;
    if (!this->metrics_enabled_) {
      queue.invalid_reasons.push_back("metrics disabled");
    } else if (nic_rx_snapshot.completion_error_count != 0) {
      queue.invalid_reasons.push_back("backend completion error");
    } else if (!nic_rx_snapshot.clock_valid) {
      queue.invalid_reasons.push_back("TSC migration or non-monotonic clock");
    } else if (!nic_rx_snapshot.mean_interval_cycles.has_value()) {
      queue.invalid_reasons.push_back("insufficient successful completions");
    } else {
      const double interval_cycles =
          *nic_rx_snapshot.mean_interval_cycles;
#ifdef AXIO_ONE_STAGE
      self_nic_rx_compl = interval_cycles / (freq * 1000.0);
#endif
      queue.measurement_valid = true;
      queue.completion_interval_cycles =
          metrics::MetricValue::from_value(interval_cycles);
      queue.completion_interval_ns =
          metrics::MetricValue::from_value(interval_cycles / freq);
      queue.completion_rate_mpps =
          metrics::MetricValue::from_value(freq * 1000.0 / interval_cycles);
      nic_rx_intervals->push_back(
          {interval_cycles, nic_rx_snapshot.timed_completion_count});
    }
    nic_rx_queues->push_back(std::move(queue));
  }

  /// AXIO_ONE_STAGE
  #ifdef AXIO_ONE_STAGE
    double max_tput = AXIO_FLOW_SIZE * kAppRequestPktsNum; //AXIO_FLOW_SIZE*(timeout_tsc/interval_tsc);
    double os_app_tx_tp  = std::min((double)1 / (self_app_tx_compl + self_app_tx_stall),max_tput);
    double os_app_rx_tp  = std::min((double)1 / (self_app_rx_compl + self_app_rx_stall),max_tput);
    double os_disp_tx_tp = std::min((double)1 / (self_disp_tx_compl + self_disp_tx_stall),max_tput);
    double os_disp_rx_tp = std::min((double)1 / (self_disp_rx_compl + self_disp_rx_stall),max_tput);
    double os_nic_tx_tp = std::min((double)1 / (self_nic_tx_compl),max_tput);
    double os_nic_rx_tp = self_nic_rx_compl > 0
                              ? std::min(1.0 / self_nic_rx_compl, max_tput)
                              : 0;
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

  if(AXIO_LIKELY(this->stats_->mbuf_allocation_count_ > 0)){
    g_stats->dispatcher_mbuf_usage_ += (double)(this->stats_->mbuf_usage_total_) / (double)(this->stats_->mbuf_allocation_count_) / (double)(Dispatcher::kMemPoolSize);
  } else {
    g_stats->dispatcher_mbuf_usage_ += 0.0f;
  }
  g_stats->app_enqueue_drop_count_ +=
      this->stats_->app_enqueue_drop_count_;
  g_stats->dispatcher_enqueue_drop_count_ +=
      this->stats_->dispatcher_enqueue_drop_count_;
  g_stats->nic_tx_packet_count_ += this->stats_->nic_tx_packet_count_;
  g_stats->nic_rx_successful_completion_count_ +=
      this->stats_->nic_rx_packet_count_;
  g_stats->nic_rx_completion_error_count_ +=
      this->stats_->nic_rx_completion_window_.snapshot()
          .completion_error_count;
}

template <class TDispatcher>
void Workspace<TDispatcher>::_mark_window_complete() {
  this->context_->completed_workspace_count_.fetch_add(
      1, std::memory_order_release);
}

template <class TDispatcher>
void Workspace<TDispatcher>::_publish_stats(uint8_t duration) {
  uint8_t worker_num = 0;
  uint8_t dispatcher_num = 0;
  double average_frequency_ghz = 0;
  std::vector<metrics::QueueCompletionInterval> nic_rx_intervals;
  std::vector<metrics::QueueMetricsRecord> nic_rx_queues;
#if AXIO_PERF_TEST_LATENCY == 1 && AXIO_NODE_TYPE == AXIO_CLIENT
  std::vector<size_t> latency_samples;
#endif
#if AXIO_CONFIG_STAGE_DISTRIBUTION_ENABLED
  std::vector<double> app_tx_allocation_stall_samples;
  std::vector<double> app_rx_handler_completion_samples;
#endif

  for (const uint8_t workspace_id : this->context_->active_workspace_ids_) {
    Workspace* workspace = this->context_->workspaces_[workspace_id];
    const double frequency_ghz = workspace->_frequency_ghz();
    workspace->_aggregate_stats(
        &this->context_->performance_stats_, frequency_ghz, duration,
        &nic_rx_intervals, &nic_rx_queues);
    if (workspace->_type() & kApplicationWorkspace) {
      worker_num++;
#if AXIO_CONFIG_STAGE_DISTRIBUTION_ENABLED
      for (const uint64_t cycles :
           workspace->app_tx_allocation_stall_sampler_.take_samples()) {
        app_tx_allocation_stall_samples.push_back(
            to_usec(cycles, frequency_ghz));
      }
      for (const uint64_t cycles :
           workspace->app_rx_handler_completion_sampler_.take_samples()) {
        app_rx_handler_completion_samples.push_back(
            to_usec(cycles, frequency_ghz));
      }
#endif
#if AXIO_PERF_TEST_LATENCY == 1 && AXIO_NODE_TYPE == AXIO_CLIENT
      for (const size_t sample : workspace->latency_samples_) {
        if (sample != 0) latency_samples.push_back(sample);
      }
#endif
    }
    if (workspace->_type() & kDispatcherWorkspace) dispatcher_num++;
    average_frequency_ghz += frequency_ghz;
  }

  rt_assert(worker_num != 0, "metrics require an application workspace");
  rt_assert(dispatcher_num != 0, "metrics require a dispatcher workspace");
  average_frequency_ghz /= this->context_->active_workspace_ids_.size();
  PerformanceStats* stats = &this->context_->performance_stats_;
  stats->app_tx_compl_ /= worker_num;
  stats->app_tx_compl_avg_ /= worker_num;
  stats->app_tx_stall_ /= worker_num;
  stats->app_tx_stall_avg_ /= worker_num;
  stats->app_rx_compl_ /= worker_num;
  stats->app_rx_compl_avg_ /= worker_num;
  stats->app_rx_stall_ /= worker_num;
  stats->app_rx_stall_avg_ /= worker_num;
  stats->disp_tx_compl_ /= dispatcher_num;
  stats->disp_tx_stall_ /= dispatcher_num;
  stats->disp_rx_compl_ /= dispatcher_num;
  stats->disp_rx_stall_ /= dispatcher_num;
  stats->nic_tx_compl_ /= dispatcher_num;
  stats->dispatcher_mbuf_usage_ /= dispatcher_num;
  stats->nic_rx_timed_completion_count_ =
      metrics::sum_timed_completion_counts(nic_rx_queues);

  const metrics::CompletionIntervalAggregate nic_rx_aggregate =
      metrics::aggregate_completion_intervals(nic_rx_intervals);
  if (nic_rx_intervals.size() == dispatcher_num &&
      nic_rx_aggregate.count_weighted_interval_cycles.has_value()) {
    stats->nic_rx_completion_valid_ = true;
    stats->nic_rx_completion_interval_cycles_ =
        *nic_rx_aggregate.count_weighted_interval_cycles;
    stats->nic_rx_slowest_interval_cycles_ =
        *nic_rx_aggregate.slowest_interval_cycles;
    stats->nic_rx_capacity_interval_cycles_ =
        *nic_rx_aggregate.aggregate_capacity_interval_cycles;
    stats->nic_rx_compl_ = stats->nic_rx_completion_interval_cycles_ /
                           (average_frequency_ghz * 1000.0);
    stats->nic_rx_slowest_compl_ = stats->nic_rx_slowest_interval_cycles_ /
                                   (average_frequency_ghz * 1000.0);
    stats->nic_rx_capacity_compl_ = stats->nic_rx_capacity_interval_cycles_ /
                                    (average_frequency_ghz * 1000.0);
  }

#if AXIO_PERF_TEST_LATENCY == 1 && AXIO_NODE_TYPE == AXIO_CLIENT
  if (!latency_samples.empty()) {
    std::sort(latency_samples.begin(), latency_samples.end());
    const size_t last = latency_samples.size() - 1;
    stats->latency_p50_us_ =
        to_usec(latency_samples[last * 50 / 100], average_frequency_ghz);
    stats->latency_p99_us_ =
        to_usec(latency_samples[last * 99 / 100], average_frequency_ghz);
    stats->latency_p999_us_ =
        to_usec(latency_samples[last * 999 / 1000], average_frequency_ghz);
    stats->latency_valid_ = true;
  }
#endif

  stats->e2e_throughput_ =
      this->context_->metrics_run_metadata_.role == "server"
          ? stats->disp_tx_throughput_
          : stats->disp_rx_throughput_;
  if (stats->e2e_throughput_ > 0) {
    stats->e2e_compl_ = 1.0 / stats->e2e_throughput_;
  }

  metrics::MetricsRecord record;
  const metrics::MetricsRunMetadata& metadata =
      this->context_->metrics_run_metadata_;
  record.run_id = metadata.run_id;
  record.role = metadata.role;
  record.backend = metadata.backend;
  record.version = metadata.version;
  record.git_commit = metadata.git_commit;
  record.build_fingerprint = metadata.build_fingerprint;
  record.config_fingerprint = metadata.config_fingerprint;
  record.duration_seconds = duration;
  record.measurement_valid = stats->nic_rx_completion_valid_;
  if (!record.measurement_valid) {
    record.invalid_reasons.push_back(
        this->metrics_enabled_ ? "one or more NIC RX queues are invalid"
                               : "metrics disabled");
  }
  record.e2e_throughput_mpps =
      metrics::MetricValue::from_value(stats->e2e_throughput_);
  if (stats->latency_valid_) {
    record.latency_p50_us =
        metrics::MetricValue::from_value(stats->latency_p50_us_);
    record.latency_p99_us =
        metrics::MetricValue::from_value(stats->latency_p99_us_);
    record.latency_p999_us =
        metrics::MetricValue::from_value(stats->latency_p999_us_);
  }
  const auto set_stage = [](metrics::StageMetricsRecord* stage,
                            double throughput, double completion,
                            double stall) {
    stage->throughput_mpps = metrics::MetricValue::from_value(throughput);
    if (throughput > 0) {
      stage->completion_time_per_packet_us =
          metrics::MetricValue::from_value(completion);
      stage->stall_time_per_packet_us =
          metrics::MetricValue::from_value(stall);
    }
  };
  set_stage(&record.app_tx, stats->app_tx_throughput_, stats->app_tx_compl_,
            stats->app_tx_stall_);
  set_stage(&record.app_rx, stats->app_rx_throughput_, stats->app_rx_compl_,
            stats->app_rx_stall_);
  set_stage(&record.dispatcher_tx, stats->disp_tx_throughput_,
            stats->disp_tx_compl_, stats->disp_tx_stall_);
  set_stage(&record.dispatcher_rx, stats->disp_rx_throughput_,
            stats->disp_rx_compl_, stats->disp_rx_stall_);
  record.nic_tx_throughput_mpps =
      metrics::MetricValue::from_value(stats->nic_tx_throughput_);
  if (stats->nic_tx_throughput_ > 0) {
    record.nic_tx_submit_time_per_packet_us =
        metrics::MetricValue::from_value(stats->nic_tx_compl_);
  }
  record.nic_rx_throughput_mpps =
      metrics::MetricValue::from_value(stats->nic_rx_throughput_);
  if (stats->nic_rx_completion_valid_) {
    record.nic_rx_completion_interval_cycles =
        metrics::MetricValue::from_value(
            stats->nic_rx_completion_interval_cycles_);
    record.nic_rx_completion_interval_ns =
        metrics::MetricValue::from_value(
            stats->nic_rx_completion_interval_cycles_ /
            average_frequency_ghz);
    record.nic_rx_slowest_interval_cycles =
        metrics::MetricValue::from_value(
            stats->nic_rx_slowest_interval_cycles_);
    record.nic_rx_capacity_interval_cycles =
        metrics::MetricValue::from_value(
            stats->nic_rx_capacity_interval_cycles_);
  }
  record.app_enqueue_drop_count = stats->app_enqueue_drop_count_;
  record.dispatcher_enqueue_drop_count =
      stats->dispatcher_enqueue_drop_count_;
  record.nic_tx_packet_count = stats->nic_tx_packet_count_;
  record.nic_rx_successful_completion_count =
      stats->nic_rx_successful_completion_count_;
  record.nic_rx_timed_completion_count =
      stats->nic_rx_timed_completion_count_;
  record.nic_rx_completion_error_count =
      stats->nic_rx_completion_error_count_;
  record.queues = std::move(nic_rx_queues);

  const uint64_t window_id =
      this->context_->metrics_publisher_->next_window_id();
  this->context_->metrics_publisher_->publish(std::move(record));
#if AXIO_CONFIG_STAGE_DISTRIBUTION_ENABLED
  metrics::StageDistributionRecord distribution_record;
  distribution_record.window_id = window_id;
  distribution_record.sample_stride =
      AXIO_CONFIG_STAGE_DISTRIBUTION_SAMPLE_STRIDE;
  distribution_record.app_tx_allocation_stall =
      metrics::summarize_distribution(
          std::move(app_tx_allocation_stall_samples));
  distribution_record.app_rx_handler_completion =
      metrics::summarize_distribution(
          std::move(app_rx_handler_completion_samples));
  this->context_->stage_distribution_writer_->append(distribution_record);
#else
  static_cast<void>(window_id);
#endif
  this->context_->_initialize_performance_stats();
  this->context_->completed_workspace_count_.store(0,
                                                   std::memory_order_release);
}

template <class TDispatcher>
void Workspace<TDispatcher>::run_event_loop_timeout_st(uint8_t iteration, uint8_t seconds) {
  size_t core_idx = get_global_index(this->numa_node_, this->ws_id_);
  /// Warmup CPU
  set_cpu_freq_max(core_idx);
  this->freq_ghz_ = measure_invariant_tsc_frequency_ghz();
#if AXIO_ROCE_MODE
  // All local verbs resources must exist before one dispatcher coordinates the
  // measurement start with the peer. The second local barrier releases every
  // workspace into the first metrics window together.
  this->_wait();
  if (this->ws_id_ == this->context_->start_sync_workspace_id_) {
    this->dispatcher_->synchronize_peer_start();
  }
  this->_wait();
#endif
  /// Sync and print stats for each one second
  for (size_t i = 0; i < iteration; i++) {
    /// Loop init
    initialize_network_stats(this->stats_);
#if AXIO_PERF_TEST_LATENCY == 1 && AXIO_NODE_TYPE == AXIO_CLIENT
    std::fill(std::begin(this->latency_samples_),
              std::end(this->latency_samples_), 0);
    this->latency_sample_index_ = 0;
#endif
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
#if AXIO_PERF_TEST_LATENCY == 1 && AXIO_NODE_TYPE == AXIO_CLIENT
    size_t lat_start_tick = start_tsc;
    size_t lat_sended_pkt_num = 0;
#endif
    while (true) {
      if (rdtsc() - loop_tsc > interval_tsc) {
        loop_tsc = rdtsc();
        launch();
        /// latency stats
      #if AXIO_PERF_TEST_LATENCY == 1 && AXIO_NODE_TYPE == AXIO_CLIENT
        if (AXIO_UNLIKELY(lat_sended_pkt_num < this->stats_->app_rx_message_count_)) {
          // 使用单次rdtscp调用优化
          size_t end_tick = kDatapathRdtsc();
          this->latency_samples_[this->latency_sample_index_] = end_tick - lat_start_tick;
          this->latency_sample_index_ = (this->latency_sample_index_ + 1) % AXIO_LATENCY_SAMPLE_COUNT;
          lat_start_tick = end_tick;  // 重用时间戳，减少一次rdtsc调用
          lat_sended_pkt_num = this->stats_->app_tx_message_count_;
        }
      #endif
      }
      if (AXIO_UNLIKELY(rdtsc() - start_tsc > timeout_tsc)) {
        this->_mark_window_complete();
        break;
      }
    }
    /* Loop End */
    /// continue loop until all workspaces are completed
    while ((this->ws_type_ & kDispatcherWorkspace) &&
           this->context_->completed_workspace_count_.load(
               std::memory_order_acquire) !=
               this->context_->active_workspace_ids_.size()) {
      launch();
      /// waiting for 100ms
      wait_tsc = rdtsc();
      while (rdtsc() - wait_tsc < ms_to_cycles(100, this->freq_ghz_)) {
        continue;
      }
    }
    this->_wait();
    if (this->ws_id_ == this->context_->active_workspace_ids_.front()) {
      this->_publish_stats(seconds);
    }
    this->_wait();
  }
#if AXIO_ROCE_MODE
  // Stop polling on every local workspace before either host destroys its QPs.
  // The persistent control connection opened by synchronize_peer_start keeps
  // this barrier independent of endpoint completion order.
  this->_wait();
  if (this->ws_id_ == this->context_->start_sync_workspace_id_) {
    this->dispatcher_->synchronize_peer_stop();
  }
  this->_wait();
#endif
  set_cpu_freq_normal(core_idx);
}

AXIO_FORCE_COMPILE_DISPATCHER
}  // namespace axio
