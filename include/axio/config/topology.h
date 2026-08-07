/**
 * @file topology.h
 * @brief Validated, indexed representation of Axio datapath topology.
 */
#pragma once

#include "axio/config/config_types.h"
#include "axio/config/config_validator.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>

namespace axio::config {

inline constexpr uint32_t kRuntimeWorkspaceLimit = 16;
inline constexpr uint32_t kRuntimeWorkloadIdLimit = 16;

class WorkspaceId {
 public:
  explicit WorkspaceId(uint32_t value) : value_(value) {}

  uint32_t value() const { return this->value_; }

  friend bool operator==(WorkspaceId left, WorkspaceId right) {
    return left.value_ == right.value_;
  }
  friend bool operator!=(WorkspaceId left, WorkspaceId right) {
    return !(left == right);
  }
  friend bool operator<(WorkspaceId left, WorkspaceId right) {
    return left.value_ < right.value_;
  }

 private:
  uint32_t value_;
};

// CpuCoreId is a zero-based core ordinal within deployment.numa_node.
class CpuCoreId {
 public:
  explicit CpuCoreId(uint32_t value) : value_(value) {}

  uint32_t value() const { return this->value_; }

  friend bool operator==(CpuCoreId left, CpuCoreId right) {
    return left.value_ == right.value_;
  }
  friend bool operator!=(CpuCoreId left, CpuCoreId right) {
    return !(left == right);
  }
  friend bool operator<(CpuCoreId left, CpuCoreId right) {
    return left.value_ < right.value_;
  }

 private:
  uint32_t value_;
};

enum class WorkspaceRole : uint8_t {
  kNone = 0,
  kApplication = 1,
  kDispatcher = 2,
};

WorkspaceRole operator|(WorkspaceRole left, WorkspaceRole right);
bool has_role(WorkspaceRole roles, WorkspaceRole role);

struct ValidatedWorkspace {
  WorkspaceId id;
  CpuCoreId cpu_core;
};

struct ValidatedGroup {
  WorkspaceId dispatcher;
  std::vector<WorkspaceId> applications;
};

struct ValidatedWorkload {
  uint32_t id;
  std::vector<PipelinePhase> pipeline;
  std::vector<WorkspaceId> remote_dispatchers;
  std::vector<ValidatedGroup> groups;
};

struct ApplicationOwner {
  uint32_t workload_id;
  size_t group_index;
  WorkspaceId dispatcher;
};

class TopologyError : public std::runtime_error {
 public:
  TopologyError(std::string key, std::string message);

  const std::string& key() const { return this->key_; }
  const std::string& message() const { return this->message_; }

 private:
  std::string key_;
  std::string message_;
};

class ValidatedTopology {
 public:
  static ValidatedTopology from_config(const AxioConfig& config);

  const std::vector<WorkspaceId>& active_workspace_ids() const {
    return this->active_workspace_ids_;
  }
  const std::vector<uint32_t>& active_workload_ids() const {
    return this->active_workload_ids_;
  }
  const ValidatedWorkspace& workspace(WorkspaceId id) const;
  const ValidatedWorkload& workload(uint32_t id) const;
  WorkspaceRole roles(WorkspaceId id) const;
  const ApplicationOwner& application_owner(WorkspaceId id) const;
  const std::vector<uint32_t>& dispatcher_workloads(WorkspaceId id) const;
  bool is_dispatcher_for_workload(WorkspaceId id, uint32_t workload_id) const;
  void validate_cpu_core_capacity(size_t available_core_count) const;

  size_t application_core_count() const {
    return this->application_owners_.size();
  }
  size_t dispatcher_queue_count() const {
    return this->dispatcher_workloads_.size();
  }

 private:
  std::vector<WorkspaceId> active_workspace_ids_;
  std::vector<uint32_t> active_workload_ids_;
  std::map<WorkspaceId, ValidatedWorkspace> workspaces_;
  std::map<uint32_t, ValidatedWorkload> workloads_;
  std::map<WorkspaceId, WorkspaceRole> roles_;
  std::map<WorkspaceId, ApplicationOwner> application_owners_;
  std::map<WorkspaceId, std::vector<uint32_t>> dispatcher_workloads_;
};

class TopologyResourcePool {
 public:
  explicit TopologyResourcePool(AxioConfig* config);

  void add_application();
  void remove_application();
  void add_dispatcher();
  void remove_dispatcher();

 private:
  AxioConfig* config_;
};

// Atomically make the explicit groups match knobs.runtime C1/C2.
void materialize_topology(AxioConfig* config);

ValidationResult validate_config_pair(const AxioConfig& local,
                                      const AxioConfig& peer);

}  // namespace axio::config
