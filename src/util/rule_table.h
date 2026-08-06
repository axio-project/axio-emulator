#pragma once

#include "common.h"
#include <vector>
#include <unordered_map>
#include <algorithm>
namespace axio {

class RuleTable {
 public:
  void add_route(uint8_t type, uint8_t ws_id) {
    this->routes_[type].push_back(ws_id);

    if (this->inflight_budgets_.count(type) == 0) {
      this->inflight_budgets_[type] = kInflightMessageBudget;
    }
  }

  bool try_acquire_inflight_budget(uint8_t type, uint64_t requested_size) {
    const bool has_budget = this->inflight_budgets_[type] >= requested_size;
    if (AXIO_UNLIKELY(has_budget)) {
      this->inflight_budgets_[type] -= requested_size;
    }
    return has_budget;
  }

  uint64_t inflight_budget(uint8_t type) {
    return this->inflight_budgets_[type];
  }

  inline void release_inflight_budget(uint8_t type, uint64_t returned_size = 1) {
    this->inflight_budgets_[type] += returned_size;
  }

  void remove_route(uint8_t type, uint8_t ws_id) {
    auto& workspace_ids = this->routes_[type];
    const auto it = std::find(workspace_ids.begin(), workspace_ids.end(), ws_id);
    if (it != workspace_ids.end()) {
      workspace_ids.erase(it);
    }
  }

  std::vector<uint8_t> workspace_ids(uint8_t type) {
    return this->routes_[type];
  }

  uint8_t select_next(uint8_t type) {
    auto& workspace_ids = this->routes_[type];
    return workspace_ids[this->next_index_++ % workspace_ids.size()];
  }

 private:
  // Workload type to destination workspace IDs and available message budget.
  std::unordered_map<uint8_t, std::vector<uint8_t>> routes_;
  std::unordered_map<uint8_t, uint64_t> inflight_budgets_;
  size_t next_index_ = 0;
};

}  // namespace axio
