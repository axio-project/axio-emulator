/**
 * @file workspace_context.h
 * @brief Shared state owned by a group of workspaces.
 */
#pragma once

#include "common.h"
#include "dispatcher.h"
#include "metrics/metrics_writer.h"
#include "metrics/stage_distribution.h"
#include "util/barrier.h"
#include "util/lock_free_queue.h"
#include "util/network_stats.h"

#include <atomic>
#include <map>
#include <mutex>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

namespace axio {

template <class TDispatcher>
class Workspace;

class WsContext {
 public:
  WsContext(ThreadBarrier* barrier, metrics::MetricsPublisher* publisher,
            metrics::StageDistributionWriter* stage_distribution_writer,
            metrics::MetricsRunMetadata run_metadata, bool metrics_enabled,
            uint8_t start_sync_workspace_id)
      : barrier_(barrier),
        metrics_publisher_(publisher),
        stage_distribution_writer_(stage_distribution_writer),
        metrics_run_metadata_(std::move(run_metadata)),
        metrics_enabled_(metrics_enabled),
        start_sync_workspace_id_(start_sync_workspace_id) {
    for (size_t i = 0; i < kWorkspaceMaxNum; ++i) {
      this->workspaces_[i] = nullptr;
    }
    initialize_performance_stats(&this->performance_stats_);
    this->random_generator_ = std::mt19937(this->random_device_());
    this->random_distribution_ = std::uniform_int_distribution<>(0, 1000);
  }

  void set_cpu_core(uint8_t workspace_id, size_t cpu_core) {
    this->cpu_cores_[workspace_id] = cpu_core;
  }

 private:
  template <class TDispatcher>
  friend class Workspace;

  void _initialize_performance_stats() {
    initialize_performance_stats(&this->performance_stats_);
  }

  Workspace<AXIO_DISPATCHER_TYPE>* workspaces_[kWorkspaceMaxNum] = {nullptr};
  std::vector<uint8_t> active_workspace_ids_;
  std::unordered_map<uint8_t, LockFreeQueue*> workspace_tx_queues_;
  std::unordered_map<uint8_t, LockFreeQueue*> workspace_rx_queues_;
  std::mutex mutex_;
  size_t cpu_cores_[kWorkspaceMaxNum] = {0};
  std::map<uint8_t, Dispatcher::MemoryRegionInfo<AXIO_MEMORY_BUFFER_TYPE>*> memory_regions_;
  std::map<uint8_t, uint8_t> workspace_dispatchers_;
  ThreadBarrier* barrier_ = nullptr;
  metrics::MetricsPublisher* metrics_publisher_ = nullptr;
  metrics::StageDistributionWriter* stage_distribution_writer_ = nullptr;
  metrics::MetricsRunMetadata metrics_run_metadata_;
  bool metrics_enabled_ = false;
  uint8_t start_sync_workspace_id_ = 0;

  std::random_device random_device_;
  std::mt19937 random_generator_;
  std::uniform_int_distribution<> random_distribution_;

  PerformanceStats performance_stats_;
  std::atomic<size_t> completed_workspace_count_{0};
};

}  // namespace axio
