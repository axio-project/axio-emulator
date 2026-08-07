/**
 * @file topology_resource_pool.cc
 * @brief Deterministic C1/C2 materialization for Axio workspace topology.
 */
#include "axio/config/topology.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace axio::config {
namespace {

struct GroupLocation {
  size_t workload_index;
  size_t group_index;
};

std::set<uint32_t> active_applications(const AxioConfig& config) {
  std::set<uint32_t> active;
  for (const WorkloadConfig& workload : config.workloads) {
    for (const WorkloadGroupConfig& group : workload.groups) {
      active.insert(group.applications.begin(), group.applications.end());
    }
  }
  return active;
}

std::set<uint32_t> active_dispatchers(const AxioConfig& config) {
  std::set<uint32_t> active;
  for (const WorkloadConfig& workload : config.workloads) {
    for (const WorkloadGroupConfig& group : workload.groups) {
      active.insert(group.dispatcher);
    }
  }
  return active;
}

void synchronize_topology_counts(AxioConfig* config) {
  config->knobs.runtime.application_core_count =
      static_cast<uint32_t>(active_applications(*config).size());
  config->knobs.runtime.dispatcher_queue_count =
      static_cast<uint32_t>(active_dispatchers(*config).size());
}

void validate_materialization_source(const AxioConfig& config) {
  AxioConfig normalized = config;
  synchronize_topology_counts(&normalized);
  static_cast<void>(ValidatedTopology::from_config(normalized));
}

std::vector<GroupLocation> group_locations(const AxioConfig& config) {
  std::vector<GroupLocation> locations;
  for (size_t workload_index = 0; workload_index < config.workloads.size();
       ++workload_index) {
    for (size_t group_index = 0;
         group_index < config.workloads[workload_index].groups.size();
         ++group_index) {
      locations.push_back({workload_index, group_index});
    }
  }
  return locations;
}

bool group_precedes(const AxioConfig& config, const GroupLocation& left,
                    const GroupLocation& right) {
  const WorkloadConfig& left_workload = config.workloads[left.workload_index];
  const WorkloadConfig& right_workload =
      config.workloads[right.workload_index];
  const WorkloadGroupConfig& left_group =
      left_workload.groups[left.group_index];
  const WorkloadGroupConfig& right_group =
      right_workload.groups[right.group_index];
  if (left_group.applications.size() != right_group.applications.size()) {
    return left_group.applications.size() < right_group.applications.size();
  }
  if (left_group.dispatcher != right_group.dispatcher) {
    return left_group.dispatcher < right_group.dispatcher;
  }
  return left_workload.id < right_workload.id;
}

GroupLocation least_loaded_group(const AxioConfig& config) {
  std::vector<GroupLocation> locations = group_locations(config);
  if (locations.empty()) {
    throw TopologyError("tuning.resources.dispatcher_workspaces",
                        "cannot assign applications without a dispatcher");
  }
  return *std::min_element(
      locations.begin(), locations.end(),
      [&](const GroupLocation& left, const GroupLocation& right) {
        return group_precedes(config, left, right);
      });
}

void assign_application(AxioConfig* config, uint32_t application) {
  const GroupLocation location = least_loaded_group(*config);
  config->workloads[location.workload_index]
      .groups[location.group_index]
      .applications.push_back(application);
}

void rebalance_applications(AxioConfig* config,
                            const std::set<uint32_t>& active) {
  for (WorkloadConfig& workload : config->workloads) {
    for (WorkloadGroupConfig& group : workload.groups) {
      group.applications.clear();
    }
  }
  for (const uint32_t application :
       config->tuning.resources.application_workspaces) {
    if (active.count(application) != 0) {
      assign_application(config, application);
    }
  }
}

void rebalance_applications(AxioConfig* config) {
  rebalance_applications(config, active_applications(*config));
}

bool has_phase(const WorkloadConfig& workload, PipelinePhase phase) {
  return std::find(workload.pipeline.begin(), workload.pipeline.end(), phase) !=
         workload.pipeline.end();
}

bool supports_materialized_group(const WorkloadConfig& workload) {
  const bool application = has_phase(workload, PipelinePhase::kApplicationTx) ||
                           has_phase(workload, PipelinePhase::kApplicationRx);
  const bool dispatcher = has_phase(workload, PipelinePhase::kDispatcherTx) ||
                          has_phase(workload, PipelinePhase::kDispatcherRx);
  return application && dispatcher;
}

size_t workload_for_new_dispatcher(const AxioConfig& config) {
  size_t selected = config.workloads.size();
  for (size_t index = 0; index < config.workloads.size(); ++index) {
    const WorkloadConfig& workload = config.workloads[index];
    if (!supports_materialized_group(workload)) continue;
    if (selected == config.workloads.size() ||
        workload.groups.size() < config.workloads[selected].groups.size() ||
        (workload.groups.size() ==
             config.workloads[selected].groups.size() &&
         workload.id < config.workloads[selected].id)) {
      selected = index;
    }
  }
  if (selected == config.workloads.size()) {
    throw TopologyError("workloads",
                        "no workload supports application/dispatcher groups");
  }
  return selected;
}

uint32_t first_inactive_resource(const std::vector<uint32_t>& resources,
                                 const std::set<uint32_t>& active,
                                 const char* key) {
  const auto candidate =
      std::find_if(resources.begin(), resources.end(), [&](uint32_t id) {
        return active.count(id) == 0;
      });
  if (candidate == resources.end()) {
    throw TopologyError(key, "resource pool is exhausted");
  }
  return *candidate;
}

uint32_t last_active_resource(const std::vector<uint32_t>& resources,
                              const std::set<uint32_t>& active,
                              const char* key) {
  const auto candidate =
      std::find_if(resources.rbegin(), resources.rend(), [&](uint32_t id) {
        return active.count(id) != 0;
      });
  if (candidate == resources.rend()) {
    throw TopologyError(key, "resource pool has no active workspace");
  }
  return *candidate;
}

void add_application_to_config(AxioConfig* config) {
  const std::set<uint32_t> active = active_applications(*config);
  const uint32_t application = first_inactive_resource(
      config->tuning.resources.application_workspaces, active,
      "tuning.resources.application_workspaces");
  assign_application(config, application);
  synchronize_topology_counts(config);
}

void remove_application_from_config(AxioConfig* config) {
  const std::set<uint32_t> active = active_applications(*config);
  const size_t dispatcher_count = active_dispatchers(*config).size();
  if (active.size() <= dispatcher_count) {
    throw TopologyError(
        "tuning.resources.application_workspaces",
        "cannot leave a dispatcher without an application workspace");
  }
  const uint32_t application = last_active_resource(
      config->tuning.resources.application_workspaces, active,
      "tuning.resources.application_workspaces");
  for (WorkloadConfig& workload : config->workloads) {
    for (WorkloadGroupConfig& group : workload.groups) {
      group.applications.erase(
          std::remove(group.applications.begin(), group.applications.end(),
                      application),
          group.applications.end());
    }
  }
  rebalance_applications(config);
  synchronize_topology_counts(config);
}

void add_dispatcher_to_config(AxioConfig* config) {
  const std::set<uint32_t> active = active_dispatchers(*config);
  if (active.size() >= active_applications(*config).size()) {
    throw TopologyError(
        "tuning.resources.dispatcher_workspaces",
        "dispatcher count cannot exceed application workspace count");
  }
  const uint32_t dispatcher = first_inactive_resource(
      config->tuning.resources.dispatcher_workspaces, active,
      "tuning.resources.dispatcher_workspaces");
  const size_t workload_index = workload_for_new_dispatcher(*config);
  config->workloads[workload_index].groups.push_back({dispatcher, {}});
  rebalance_applications(config);
  synchronize_topology_counts(config);
}

void remove_dispatcher_from_config(AxioConfig* config) {
  const std::set<uint32_t> active = active_dispatchers(*config);
  const std::set<uint32_t> applications = active_applications(*config);
  if (active.size() <= 1) {
    throw TopologyError("tuning.resources.dispatcher_workspaces",
                        "cannot remove the final dispatcher workspace");
  }
  const uint32_t dispatcher = last_active_resource(
      config->tuning.resources.dispatcher_workspaces, active,
      "tuning.resources.dispatcher_workspaces");
  for (WorkloadConfig& workload : config->workloads) {
    workload.groups.erase(
        std::remove_if(workload.groups.begin(), workload.groups.end(),
                       [&](const WorkloadGroupConfig& group) {
                         return group.dispatcher == dispatcher;
                       }),
        workload.groups.end());
  }
  rebalance_applications(config, applications);
  synchronize_topology_counts(config);
}

}  // namespace

TopologyResourcePool::TopologyResourcePool(AxioConfig* config)
    : config_(config) {
  if (this->config_ == nullptr) {
    throw std::invalid_argument("topology resource pool requires a config");
  }
  validate_materialization_source(*this->config_);
}

void TopologyResourcePool::add_application() {
  AxioConfig candidate = *this->config_;
  add_application_to_config(&candidate);
  validate_materialization_source(candidate);
  *this->config_ = std::move(candidate);
}

void TopologyResourcePool::remove_application() {
  AxioConfig candidate = *this->config_;
  remove_application_from_config(&candidate);
  validate_materialization_source(candidate);
  *this->config_ = std::move(candidate);
}

void TopologyResourcePool::add_dispatcher() {
  AxioConfig candidate = *this->config_;
  add_dispatcher_to_config(&candidate);
  validate_materialization_source(candidate);
  *this->config_ = std::move(candidate);
}

void TopologyResourcePool::remove_dispatcher() {
  AxioConfig candidate = *this->config_;
  remove_dispatcher_from_config(&candidate);
  validate_materialization_source(candidate);
  *this->config_ = std::move(candidate);
}

void materialize_topology(AxioConfig* config) {
  if (config == nullptr) {
    throw std::invalid_argument("topology materialization requires a config");
  }
  const uint32_t target_applications =
      config->knobs.runtime.application_core_count;
  const uint32_t target_dispatchers =
      config->knobs.runtime.dispatcher_queue_count;
  if (target_applications < target_dispatchers) {
    throw TopologyError("knobs.runtime.dispatcher_queue_count",
                        "must not exceed application_core_count");
  }

  AxioConfig candidate = *config;
  TopologyResourcePool resources(&candidate);
  while (active_applications(candidate).size() < target_applications) {
    resources.add_application();
  }
  while (active_dispatchers(candidate).size() < target_dispatchers) {
    resources.add_dispatcher();
  }
  while (active_dispatchers(candidate).size() > target_dispatchers) {
    resources.remove_dispatcher();
  }
  while (active_applications(candidate).size() > target_applications) {
    resources.remove_application();
  }
  static_cast<void>(ValidatedTopology::from_config(candidate));
  *config = std::move(candidate);
}

}  // namespace axio::config
