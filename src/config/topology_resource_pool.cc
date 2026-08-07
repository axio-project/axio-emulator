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
                            size_t workload_index,
                            const std::vector<uint32_t>& applications) {
  WorkloadConfig& workload = config->workloads[workload_index];
  for (WorkloadGroupConfig& group : workload.groups) {
    group.applications.clear();
  }
  for (const uint32_t application :
       config->tuning.resources.application_workspaces) {
    if (std::find(applications.begin(), applications.end(), application) ==
        applications.end()) {
      continue;
    }
    std::vector<GroupLocation> locations;
    for (size_t group_index = 0; group_index < workload.groups.size();
         ++group_index) {
      locations.push_back({workload_index, group_index});
    }
    if (locations.empty()) {
      throw TopologyError("workloads",
                          "cannot preserve workload ownership without a "
                          "dispatcher group");
    }
    const GroupLocation location = *std::min_element(
        locations.begin(), locations.end(),
        [&](const GroupLocation& left, const GroupLocation& right) {
          return group_precedes(*config, left, right);
        });
    workload.groups[location.group_index].applications.push_back(application);
  }
}

std::vector<uint32_t> workload_applications(const WorkloadConfig& workload) {
  std::vector<uint32_t> applications;
  for (const WorkloadGroupConfig& group : workload.groups) {
    applications.insert(applications.end(), group.applications.begin(),
                        group.applications.end());
  }
  return applications;
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
    const size_t application_count = workload_applications(workload).size();
    if (!supports_materialized_group(workload) || workload.groups.empty() ||
        application_count <= workload.groups.size()) {
      continue;
    }
    const size_t spare_count = application_count - workload.groups.size();
    if (selected == config.workloads.size()) {
      selected = index;
      continue;
    }
    const WorkloadConfig& current = config.workloads[selected];
    const size_t current_spare =
        workload_applications(current).size() - current.groups.size();
    if (spare_count > current_spare ||
        (spare_count == current_spare && workload.id < current.id)) {
      selected = index;
    }
  }
  if (selected == config.workloads.size()) {
    throw TopologyError(
        "tuning.resources.dispatcher_workspaces",
        "no workload has more applications than dispatcher groups");
  }
  return selected;
}

size_t application_workload(const AxioConfig& config, uint32_t application) {
  for (size_t workload_index = 0; workload_index < config.workloads.size();
       ++workload_index) {
    for (const WorkloadGroupConfig& group :
         config.workloads[workload_index].groups) {
      if (std::find(group.applications.begin(), group.applications.end(),
                    application) != group.applications.end()) {
        return workload_index;
      }
    }
  }
  throw TopologyError("tuning.resources.application_workspaces",
                      "active application has no workload owner");
}

void require_unreused_dispatchers(const AxioConfig& config) {
  const size_t group_count = group_locations(config).size();
  if (group_count != active_dispatchers(config).size()) {
    throw TopologyError(
        "tuning.resources.dispatcher_workspaces",
        "dispatcher add/remove is ambiguous when a workspace is reused "
        "across workload groups");
  }
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
  if (active.size() <= 1) {
    throw TopologyError(
        "tuning.resources.application_workspaces",
        "cannot remove the final application workspace");
  }
  const uint32_t application = last_active_resource(
      config->tuning.resources.application_workspaces, active,
      "tuning.resources.application_workspaces");
  const size_t workload_index = application_workload(*config, application);
  for (WorkloadConfig& workload : config->workloads) {
    for (WorkloadGroupConfig& group : workload.groups) {
      group.applications.erase(
          std::remove(group.applications.begin(), group.applications.end(),
                      application),
          group.applications.end());
    }
  }
  const std::vector<uint32_t> remaining =
      workload_applications(config->workloads[workload_index]);
  rebalance_applications(config, workload_index, remaining);
  synchronize_topology_counts(config);
}

void add_dispatcher_to_config(AxioConfig* config) {
  require_unreused_dispatchers(*config);
  const std::set<uint32_t> active = active_dispatchers(*config);
  if (group_locations(*config).size() >= active_applications(*config).size()) {
    throw TopologyError(
        "tuning.resources.dispatcher_workspaces",
        "dispatcher count cannot exceed application workspace count");
  }
  const uint32_t dispatcher = first_inactive_resource(
      config->tuning.resources.dispatcher_workspaces, active,
      "tuning.resources.dispatcher_workspaces");
  const size_t workload_index = workload_for_new_dispatcher(*config);
  const std::vector<uint32_t> applications =
      workload_applications(config->workloads[workload_index]);
  config->workloads[workload_index].groups.push_back({dispatcher, {}});
  rebalance_applications(config, workload_index, applications);
  synchronize_topology_counts(config);
}

void remove_dispatcher_from_config(AxioConfig* config) {
  require_unreused_dispatchers(*config);
  const std::set<uint32_t> active = active_dispatchers(*config);
  if (active.size() <= 1) {
    throw TopologyError("tuning.resources.dispatcher_workspaces",
                        "cannot remove the final dispatcher workspace");
  }
  const uint32_t dispatcher = last_active_resource(
      config->tuning.resources.dispatcher_workspaces, active,
      "tuning.resources.dispatcher_workspaces");
  size_t workload_index = config->workloads.size();
  size_t group_index = 0;
  for (size_t index = 0; index < config->workloads.size(); ++index) {
    const auto group = std::find_if(
        config->workloads[index].groups.begin(),
        config->workloads[index].groups.end(),
        [&](const WorkloadGroupConfig& value) {
          return value.dispatcher == dispatcher;
        });
    if (group != config->workloads[index].groups.end()) {
      workload_index = index;
      group_index = static_cast<size_t>(
          std::distance(config->workloads[index].groups.begin(), group));
      break;
    }
  }
  if (workload_index == config->workloads.size()) {
    throw TopologyError("tuning.resources.dispatcher_workspaces",
                        "active dispatcher has no workload group");
  }
  WorkloadConfig& workload = config->workloads[workload_index];
  const std::vector<uint32_t> applications = workload_applications(workload);
  if (workload.groups.size() == 1 && !applications.empty()) {
    throw TopologyError(
        "tuning.resources.dispatcher_workspaces",
        "cannot remove the only dispatcher group for an active workload");
  }
  workload.groups.erase(workload.groups.begin() +
                        static_cast<std::ptrdiff_t>(group_index));
  if (!applications.empty()) {
    rebalance_applications(config, workload_index, applications);
  }
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
  validate_materialization_source(candidate);
  while (active_applications(candidate).size() < target_applications) {
    add_application_to_config(&candidate);
  }
  while (active_dispatchers(candidate).size() < target_dispatchers) {
    add_dispatcher_to_config(&candidate);
  }
  while (active_applications(candidate).size() > target_applications) {
    remove_application_from_config(&candidate);
  }
  while (active_dispatchers(candidate).size() > target_dispatchers) {
    remove_dispatcher_from_config(&candidate);
  }
  static_cast<void>(ValidatedTopology::from_config(candidate));
  *config = std::move(candidate);
}

void materialize_topology_pair(AxioConfig* local, AxioConfig* peer) {
  if (local == nullptr || peer == nullptr) {
    throw std::invalid_argument(
        "topology pair materialization requires two configs");
  }
  AxioConfig local_candidate = *local;
  AxioConfig peer_candidate = *peer;
  materialize_topology(&local_candidate);
  materialize_topology(&peer_candidate);

  const auto synchronize_remote_routes = [](AxioConfig* destination,
                                             const AxioConfig& source) {
    for (WorkloadConfig& destination_workload : destination->workloads) {
      const auto source_workload = std::find_if(
          source.workloads.begin(), source.workloads.end(),
          [&](const WorkloadConfig& value) {
            return value.id == destination_workload.id;
          });
      if (source_workload == source.workloads.end()) continue;
      destination_workload.remote_dispatchers.clear();
      for (const WorkloadGroupConfig& group : source_workload->groups) {
        if (std::find(destination_workload.remote_dispatchers.begin(),
                      destination_workload.remote_dispatchers.end(),
                      group.dispatcher) ==
            destination_workload.remote_dispatchers.end()) {
          destination_workload.remote_dispatchers.push_back(group.dispatcher);
        }
      }
    }
  };
  synchronize_remote_routes(&local_candidate, peer_candidate);
  synchronize_remote_routes(&peer_candidate, local_candidate);

  const ValidationResult validation =
      validate_config_pair(local_candidate, peer_candidate);
  if (!validation.ok()) {
    throw TopologyError("workloads.remote_dispatchers", validation.format());
  }
  *local = std::move(local_candidate);
  *peer = std::move(peer_candidate);
}

}  // namespace axio::config
