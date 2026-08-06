/**
 * @file workspace_context.h
 * @brief Shared state owned by a group of workspaces.
 */
#pragma once

#include "common.h"
#include "dispatcher.h"
#include "util/barrier.h"
#include "util/lock_free_queue.h"
#include "util/net_stats.h"

#include <map>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>

namespace axio {

template <class TDispatcher>
class Workspace;

class WsContext {
 public:
  explicit WsContext(ThreadBarrier* barrier) : barrier_(barrier) {
    for (size_t i = 0; i < kWorkspaceMaxNum; ++i) {
      this->workspaces_[i] = nullptr;
    }
    perf_stats_init(&this->performance_stats_);
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
    perf_stats_init(&this->performance_stats_);
  }

  Workspace<DISPATCHER_TYPE>* workspaces_[kWorkspaceMaxNum] = {nullptr};
  std::vector<uint8_t> active_workspace_ids_;
  std::unordered_map<uint8_t, LockFreeQueue*> workspace_tx_queues_;
  std::unordered_map<uint8_t, LockFreeQueue*> workspace_rx_queues_;
  std::mutex mutex_;
  size_t cpu_cores_[kWorkspaceMaxNum] = {0};
  std::map<uint8_t, Dispatcher::mem_reg_info<MEM_REG_TYPE>*> memory_regions_;
  std::map<uint8_t, uint8_t> workspace_dispatchers_;
  ThreadBarrier* barrier_ = nullptr;

  std::random_device random_device_;
  std::mt19937 random_generator_;
  std::uniform_int_distribution<> random_distribution_;

  perf_stats performance_stats_;
  volatile bool end_signal_ = false;
  volatile uint8_t completed_workspace_count_ = 0;
};

}  // namespace axio
