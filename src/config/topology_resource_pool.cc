/**
 * @file topology_resource_pool.cc
 * @brief Deterministic C1/C2 materialization for Axio workspace topology.
 */
#include "axio/config/topology.h"

#include <algorithm>
#include <map>
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
  for (const WorkloadConfig& workload : config.deployment.topology.workloads) {
    for (const WorkloadGroupConfig& group : workload.groups) {
      active.insert(group.applications.begin(), group.applications.end());
    }
  }
  return active;
}

std::set<uint32_t> active_dispatchers(const AxioConfig& config) {
  std::set<uint32_t> active;
  for (const WorkloadConfig& workload : config.deployment.topology.workloads) {
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
  for (size_t workload_index = 0; workload_index < config.deployment.topology.workloads.size();
       ++workload_index) {
    for (size_t group_index = 0;
         group_index < config.deployment.topology.workloads[workload_index].groups.size();
         ++group_index) {
      locations.push_back({workload_index, group_index});
    }
  }
  return locations;
}

bool group_precedes(const AxioConfig& config, const GroupLocation& left,
                    const GroupLocation& right) {
  const WorkloadConfig& left_workload = config.deployment.topology.workloads[left.workload_index];
  const WorkloadConfig& right_workload =
      config.deployment.topology.workloads[right.workload_index];
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
    throw TopologyError("deployment.topology.dispatcher_workspaces",
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
  config->deployment.topology.workloads[location.workload_index]
      .groups[location.group_index]
      .applications.push_back(application);
}

void rebalance_applications(AxioConfig* config,
                            size_t workload_index,
                            const std::vector<uint32_t>& applications) {
  WorkloadConfig& workload = config->deployment.topology.workloads[workload_index];
  for (WorkloadGroupConfig& group : workload.groups) {
    group.applications.clear();
  }
  for (const uint32_t application :
       config->deployment.topology.application_workspaces) {
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
      throw TopologyError("deployment.topology.workloads",
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
  size_t selected = config.deployment.topology.workloads.size();
  for (size_t index = 0; index < config.deployment.topology.workloads.size(); ++index) {
    const WorkloadConfig& workload = config.deployment.topology.workloads[index];
    const size_t application_count = workload_applications(workload).size();
    if (!supports_materialized_group(workload) || workload.groups.empty() ||
        application_count <= workload.groups.size()) {
      continue;
    }
    const size_t spare_count = application_count - workload.groups.size();
    if (selected == config.deployment.topology.workloads.size()) {
      selected = index;
      continue;
    }
    const WorkloadConfig& current = config.deployment.topology.workloads[selected];
    const size_t current_spare =
        workload_applications(current).size() - current.groups.size();
    if (spare_count > current_spare ||
        (spare_count == current_spare && workload.id < current.id)) {
      selected = index;
    }
  }
  if (selected == config.deployment.topology.workloads.size()) {
    throw TopologyError(
        "deployment.topology.dispatcher_workspaces",
        "no workload has more applications than dispatcher groups");
  }
  return selected;
}

size_t application_workload(const AxioConfig& config, uint32_t application) {
  for (size_t workload_index = 0; workload_index < config.deployment.topology.workloads.size();
       ++workload_index) {
    for (const WorkloadGroupConfig& group :
         config.deployment.topology.workloads[workload_index].groups) {
      if (std::find(group.applications.begin(), group.applications.end(),
                    application) != group.applications.end()) {
        return workload_index;
      }
    }
  }
  throw TopologyError("deployment.topology.application_workspaces",
                      "active application has no workload owner");
}

std::map<uint32_t, std::vector<GroupLocation>> dispatcher_groups(
    const AxioConfig& config) {
  std::map<uint32_t, std::vector<GroupLocation>> groups;
  for (const GroupLocation& location : group_locations(config)) {
    const WorkloadGroupConfig& group =
        config.deployment.topology.workloads[location.workload_index].groups[location.group_index];
    groups[group.dispatcher].push_back(location);
  }
  return groups;
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
      config->deployment.topology.application_workspaces, active,
      "deployment.topology.application_workspaces");
  assign_application(config, application);
  synchronize_topology_counts(config);
}

void remove_application_from_config(AxioConfig* config) {
  const std::set<uint32_t> active = active_applications(*config);
  if (active.size() <= 1) {
    throw TopologyError(
        "deployment.topology.application_workspaces",
        "cannot remove the final application workspace");
  }
  const uint32_t application = last_active_resource(
      config->deployment.topology.application_workspaces, active,
      "deployment.topology.application_workspaces");
  const size_t workload_index = application_workload(*config, application);
  for (WorkloadConfig& workload : config->deployment.topology.workloads) {
    for (WorkloadGroupConfig& group : workload.groups) {
      group.applications.erase(
          std::remove(group.applications.begin(), group.applications.end(),
                      application),
          group.applications.end());
    }
  }
  const std::vector<uint32_t> remaining =
      workload_applications(config->deployment.topology.workloads[workload_index]);
  rebalance_applications(config, workload_index, remaining);
  synchronize_topology_counts(config);
}

void add_dispatcher_to_config(AxioConfig* config) {
  const std::set<uint32_t> active = active_dispatchers(*config);
  const uint32_t dispatcher = first_inactive_resource(
      config->deployment.topology.dispatcher_workspaces, active,
      "deployment.topology.dispatcher_workspaces");
  const std::map<uint32_t, std::vector<GroupLocation>> assignments =
      dispatcher_groups(*config);
  const auto reused = std::max_element(
      assignments.begin(), assignments.end(),
      [](const auto& left, const auto& right) {
        if (left.second.size() != right.second.size()) {
          return left.second.size() < right.second.size();
        }
        return left.first > right.first;
      });
  if (reused != assignments.end() && reused->second.size() > 1) {
    const GroupLocation selected = *std::max_element(
        reused->second.begin(), reused->second.end(),
        [&](const GroupLocation& left, const GroupLocation& right) {
          const WorkloadConfig& left_workload =
              config->deployment.topology.workloads[left.workload_index];
          const WorkloadConfig& right_workload =
              config->deployment.topology.workloads[right.workload_index];
          const WorkloadGroupConfig& left_group =
              left_workload.groups[left.group_index];
          const WorkloadGroupConfig& right_group =
              right_workload.groups[right.group_index];
          if (left_group.applications.size() !=
              right_group.applications.size()) {
            return left_group.applications.size() <
                   right_group.applications.size();
          }
          return left_workload.id > right_workload.id;
        });
    WorkloadConfig& workload = config->deployment.topology.workloads[selected.workload_index];
    const std::vector<uint32_t> applications =
        workload_applications(workload);
    workload.groups[selected.group_index].dispatcher = dispatcher;
    rebalance_applications(config, selected.workload_index, applications);
    synchronize_topology_counts(config);
    return;
  }
  if (group_locations(*config).size() >= active_applications(*config).size()) {
    throw TopologyError(
        "deployment.topology.dispatcher_workspaces",
        "dispatcher count cannot exceed application workspace count");
  }
  const size_t workload_index = workload_for_new_dispatcher(*config);
  const std::vector<uint32_t> applications =
      workload_applications(config->deployment.topology.workloads[workload_index]);
  config->deployment.topology.workloads[workload_index].groups.push_back({dispatcher, {}});
  rebalance_applications(config, workload_index, applications);
  synchronize_topology_counts(config);
}

void remove_dispatcher_from_config(AxioConfig* config) {
  const std::set<uint32_t> active = active_dispatchers(*config);
  if (active.size() <= 1) {
    throw TopologyError("deployment.topology.dispatcher_workspaces",
                        "cannot remove the final dispatcher workspace");
  }
  const uint32_t dispatcher = last_active_resource(
      config->deployment.topology.dispatcher_workspaces, active,
      "deployment.topology.dispatcher_workspaces");
  std::map<uint32_t, size_t> assignment_counts;
  for (const uint32_t survivor : active) {
    if (survivor != dispatcher) assignment_counts[survivor] = 0;
  }
  for (const WorkloadConfig& workload : config->deployment.topology.workloads) {
    for (const WorkloadGroupConfig& group : workload.groups) {
      if (group.dispatcher != dispatcher) {
        ++assignment_counts[group.dispatcher];
      }
    }
  }
  for (size_t workload_index = 0; workload_index < config->deployment.topology.workloads.size();
       ++workload_index) {
    WorkloadConfig& workload = config->deployment.topology.workloads[workload_index];
    const size_t original_group_count = workload.groups.size();
    const size_t removed_group_count = static_cast<size_t>(std::count_if(
        workload.groups.begin(), workload.groups.end(),
        [&](const WorkloadGroupConfig& group) {
          return group.dispatcher == dispatcher;
        }));
    if (removed_group_count == 0) continue;
    const std::vector<uint32_t> applications =
        workload_applications(workload);
    workload.groups.erase(
        std::remove_if(workload.groups.begin(), workload.groups.end(),
                       [&](const WorkloadGroupConfig& group) {
                         return group.dispatcher == dispatcher;
                       }),
        workload.groups.end());
    const size_t desired_group_count =
        std::min({original_group_count, applications.size(),
                  assignment_counts.size()});
    while (workload.groups.size() < desired_group_count) {
      const auto replacement = std::min_element(
          assignment_counts.begin(), assignment_counts.end(),
          [&](const auto& left, const auto& right) {
            const auto already_serves = [&](uint32_t candidate) {
              return std::any_of(
                  workload.groups.begin(), workload.groups.end(),
                  [&](const WorkloadGroupConfig& group) {
                    return group.dispatcher == candidate;
                  });
            };
            const bool left_used = already_serves(left.first);
            const bool right_used = already_serves(right.first);
            if (left_used != right_used) return !left_used;
            if (left.second != right.second) return left.second < right.second;
            return left.first < right.first;
          });
      if (replacement == assignment_counts.end()) {
        throw TopologyError("deployment.topology.dispatcher_workspaces",
                            "cannot remove the final dispatcher workspace");
      }
      workload.groups.push_back({replacement->first, {}});
      ++replacement->second;
    }
    if (!applications.empty()) {
      rebalance_applications(config, workload_index, applications);
    }
  }
  synchronize_topology_counts(config);
}

void synchronize_remote_routes(AxioConfig* destination,
                               const AxioConfig& source) {
  for (WorkloadConfig& destination_workload :
       destination->deployment.topology.workloads) {
    const auto source_workload = std::find_if(
        source.deployment.topology.workloads.begin(),
        source.deployment.topology.workloads.end(),
        [&](const WorkloadConfig& value) {
          return value.id == destination_workload.id;
        });
    if (source_workload == source.deployment.topology.workloads.end()) {
      continue;
    }
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
}

std::vector<uint32_t> ordered_active_resources(
    const std::vector<uint32_t>& resources,
    const std::set<uint32_t>& active) {
  std::vector<uint32_t> ordered;
  for (const uint32_t resource : resources) {
    if (active.count(resource) != 0) ordered.push_back(resource);
  }
  return ordered;
}

std::vector<uint32_t> ordered_intersection(
    const std::vector<uint32_t>& left,
    const std::vector<uint32_t>& right) {
  const std::set<uint32_t> right_ids(right.begin(), right.end());
  std::vector<uint32_t> intersection;
  for (const uint32_t id : left) {
    if (right_ids.count(id) != 0) intersection.push_back(id);
  }
  return intersection;
}

std::vector<uint32_t> take_prefix(const std::vector<uint32_t>& resources,
                                  size_t count, const char* key) {
  if (resources.size() < count) {
    throw TopologyError(key, "resource pool is exhausted for topology profile");
  }
  return std::vector<uint32_t>(resources.begin(), resources.begin() + count);
}

struct ProfileRoles {
  std::vector<uint32_t> applications;
  std::vector<uint32_t> dispatchers;
  size_t fanout;
};

ProfileRoles select_profile_roles(const AxioConfig& config,
                                  TopologySearchProfile profile) {
  const size_t application_count =
      config.knobs.runtime.application_core_count;
  const size_t dispatcher_count =
      config.knobs.runtime.dispatcher_queue_count;
  if (application_count == 0 || dispatcher_count == 0) {
    throw TopologyError("knobs.runtime.application_core_count",
                        "topology profiles require positive C1 and C2");
  }

  const std::vector<uint32_t> colocated = ordered_intersection(
      config.deployment.topology.application_workspaces,
      config.deployment.topology.dispatcher_workspaces);
  if (profile == TopologySearchProfile::kColocatedOneToOne) {
    if (application_count != dispatcher_count) {
      throw TopologyError("knobs.runtime.application_core_count",
                          "colocated-1to1 requires C1 equal to C2");
    }
    const std::vector<uint32_t> selected = take_prefix(
        colocated, application_count,
        "deployment.topology.application_workspaces");
    return {selected, selected, 1};
  }

  if (profile == TopologySearchProfile::kSplitOneToOne) {
    if (application_count != dispatcher_count) {
      throw TopologyError("knobs.runtime.application_core_count",
                          "split-1to1 requires C1 equal to C2");
    }
    const std::vector<uint32_t> applications = take_prefix(
        config.deployment.topology.application_workspaces, application_count,
        "deployment.topology.application_workspaces");
    const std::set<uint32_t> application_ids(applications.begin(),
                                             applications.end());
    std::vector<uint32_t> available_dispatchers;
    for (const uint32_t dispatcher :
         config.deployment.topology.dispatcher_workspaces) {
      if (application_ids.count(dispatcher) == 0) {
        available_dispatchers.push_back(dispatcher);
      }
    }
    return {applications,
            take_prefix(available_dispatchers, dispatcher_count,
                        "deployment.topology.dispatcher_workspaces"),
            1};
  }

  if (application_count < dispatcher_count ||
      application_count % dispatcher_count != 0) {
    throw TopologyError(
        "knobs.runtime.application_core_count",
        "colocated-fanout requires C1 >= C2 and C1 divisible by C2");
  }
  const std::vector<uint32_t> dispatchers = take_prefix(
      colocated, dispatcher_count,
      "deployment.topology.dispatcher_workspaces");
  std::vector<uint32_t> applications = dispatchers;
  const std::set<uint32_t> base_ids(dispatchers.begin(), dispatchers.end());
  for (const uint32_t application :
       config.deployment.topology.application_workspaces) {
    if (base_ids.count(application) == 0) {
      applications.push_back(application);
    }
    if (applications.size() == application_count) break;
  }
  if (applications.size() != application_count) {
    throw TopologyError("deployment.topology.application_workspaces",
                        "resource pool is exhausted for topology profile");
  }
  return {applications, dispatchers, application_count / dispatcher_count};
}

void rebalance_profile_groups(AxioConfig* config, size_t fanout,
                              size_t dispatcher_count) {
  const std::vector<GroupLocation> locations = group_locations(*config);
  if (locations.size() != dispatcher_count) {
    throw TopologyError(
        "deployment.topology.workloads",
        "topology profile requires one group per active dispatcher");
  }
  for (size_t workload_index = 0;
       workload_index < config->deployment.topology.workloads.size();
       ++workload_index) {
    WorkloadConfig& workload =
        config->deployment.topology.workloads[workload_index];
    const std::vector<uint32_t> applications =
        workload_applications(workload);
    if (applications.size() != workload.groups.size() * fanout) {
      throw TopologyError(
          "deployment.topology.workloads",
          "cannot preserve workload ownership with balanced profile groups");
    }
    rebalance_applications(config, workload_index, applications);
  }
}

void remap_profile_roles(AxioConfig* config, const ProfileRoles& roles) {
  const std::vector<uint32_t> current_applications = ordered_active_resources(
      config->deployment.topology.application_workspaces,
      active_applications(*config));
  const std::vector<uint32_t> current_dispatchers = ordered_active_resources(
      config->deployment.topology.dispatcher_workspaces,
      active_dispatchers(*config));
  if (current_applications.size() != roles.applications.size() ||
      current_dispatchers.size() != roles.dispatchers.size()) {
    throw TopologyError("deployment.topology.workloads",
                        "materialized topology profile count mismatch");
  }
  std::map<uint32_t, uint32_t> application_remap;
  std::map<uint32_t, uint32_t> dispatcher_remap;
  for (size_t index = 0; index < current_applications.size(); ++index) {
    application_remap.emplace(current_applications[index],
                              roles.applications[index]);
  }
  for (size_t index = 0; index < current_dispatchers.size(); ++index) {
    dispatcher_remap.emplace(current_dispatchers[index],
                             roles.dispatchers[index]);
  }
  for (WorkloadConfig& workload : config->deployment.topology.workloads) {
    for (WorkloadGroupConfig& group : workload.groups) {
      group.dispatcher = dispatcher_remap.at(group.dispatcher);
      for (uint32_t& application : group.applications) {
        application = application_remap.at(application);
      }
    }
  }
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
  synchronize_remote_routes(&local_candidate, peer_candidate);
  synchronize_remote_routes(&peer_candidate, local_candidate);

  const ValidationResult validation =
      validate_config_pair(local_candidate, peer_candidate);
  if (!validation.ok()) {
    throw TopologyError("deployment.topology.workloads.remote_dispatchers",
                        validation.format());
  }
  *local = std::move(local_candidate);
  *peer = std::move(peer_candidate);
}

void materialize_target_topology_pair(AxioConfig* target, AxioConfig* peer) {
  if (target == nullptr || peer == nullptr) {
    throw std::invalid_argument(
        "target topology materialization requires two configs");
  }
  AxioConfig target_candidate = *target;
  AxioConfig peer_candidate = *peer;
  materialize_topology(&target_candidate);
  synchronize_remote_routes(&target_candidate, peer_candidate);
  synchronize_remote_routes(&peer_candidate, target_candidate);

  const ValidationResult validation =
      validate_config_pair(target_candidate, peer_candidate);
  if (!validation.ok()) {
    throw TopologyError("deployment.topology.workloads.remote_dispatchers",
                        validation.format());
  }
  *target = std::move(target_candidate);
  *peer = std::move(peer_candidate);
}

void materialize_target_topology_profile_pair(
    AxioConfig* target, AxioConfig* peer, TopologySearchProfile profile) {
  if (target == nullptr || peer == nullptr) {
    throw std::invalid_argument(
        "target topology profile materialization requires two configs");
  }
  AxioConfig target_candidate = *target;
  AxioConfig peer_candidate = *peer;
  const ProfileRoles roles = select_profile_roles(target_candidate, profile);
  materialize_topology(&target_candidate);
  rebalance_profile_groups(&target_candidate, roles.fanout,
                           roles.dispatchers.size());
  remap_profile_roles(&target_candidate, roles);
  static_cast<void>(ValidatedTopology::from_config(target_candidate));
  synchronize_remote_routes(&target_candidate, peer_candidate);
  synchronize_remote_routes(&peer_candidate, target_candidate);

  const ValidationResult validation =
      validate_config_pair(target_candidate, peer_candidate);
  if (!validation.ok()) {
    throw TopologyError("deployment.topology.workloads.remote_dispatchers",
                        validation.format());
  }
  *target = std::move(target_candidate);
  *peer = std::move(peer_candidate);
}

}  // namespace axio::config
