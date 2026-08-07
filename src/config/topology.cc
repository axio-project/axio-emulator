/**
 * @file topology.cc
 * @brief Build and validate indexed Axio datapath topology.
 */
#include "axio/config/topology.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace axio::config {
namespace {

std::string workspace_key(size_t index, const char* field) {
  return "workspaces[" + std::to_string(index) + "]." + field;
}

std::string workload_key(size_t index, const char* field) {
  return "workloads[" + std::to_string(index) + "]." + field;
}

std::string group_key(size_t workload_index, size_t group_index,
                      const char* field) {
  return "workloads[" + std::to_string(workload_index) + "].groups[" +
         std::to_string(group_index) + "]." + field;
}

SourceLocation source_for(const AxioConfig& config, const std::string& key) {
  const auto source = config.source_locations.find(key);
  if (source != config.source_locations.end()) {
    return source->second;
  }
  return {config.source_path.string(), 1, 1};
}

bool is_valid_phase(PipelinePhase phase) {
  switch (phase) {
    case PipelinePhase::kApplicationTx:
    case PipelinePhase::kDispatcherTx:
    case PipelinePhase::kNicTx:
    case PipelinePhase::kNicRx:
    case PipelinePhase::kDispatcherRx:
    case PipelinePhase::kApplicationRx: return true;
  }
  return false;
}

std::set<WorkspaceId> validate_resource_pool(
    const std::vector<uint32_t>& values, const char* key,
    const std::map<WorkspaceId, ValidatedWorkspace>& workspaces) {
  std::set<WorkspaceId> seen;
  for (const uint32_t value : values) {
    const WorkspaceId id(value);
    if (workspaces.find(id) == workspaces.end()) {
      throw TopologyError(key, "references undefined workspace " +
                                  std::to_string(value));
    }
    if (!seen.insert(id).second) {
      throw TopologyError(key, "contains duplicate workspace " +
                                  std::to_string(value));
    }
  }
  return seen;
}

void append_pair_issues(const AxioConfig& source,
                        const ValidatedTopology& source_topology,
                        const ValidatedTopology& peer_topology,
                        std::vector<ValidationIssue>* issues) {
  for (size_t workload_index = 0; workload_index < source.workloads.size();
       ++workload_index) {
    const WorkloadConfig& workload = source.workloads[workload_index];
    const std::string key =
        workload_key(workload_index, "remote_dispatchers");
    const ValidatedWorkload& validated_workload =
        source_topology.workload(workload.id);
    if (validated_workload.groups.empty()) continue;
    std::set<uint32_t> configured_remote_ids;
    for (const uint32_t remote_id : workload.remote_dispatchers) {
      configured_remote_ids.insert(remote_id);
      if (!peer_topology.is_dispatcher_for_workload(WorkspaceId(remote_id),
                                                    workload.id)) {
        std::ostringstream message;
        message << "peer workspace " << remote_id
                << " is not a dispatcher for workload " << workload.id;
        issues->push_back({key, source_for(source, key), message.str()});
      }
    }
    try {
      const ValidatedWorkload& peer_workload =
          peer_topology.workload(workload.id);
      std::set<uint32_t> peer_dispatcher_ids;
      for (const ValidatedGroup& group : peer_workload.groups) {
        peer_dispatcher_ids.insert(group.dispatcher.value());
      }
      for (const uint32_t peer_dispatcher : peer_dispatcher_ids) {
        if (configured_remote_ids.count(peer_dispatcher) == 0) {
          issues->push_back(
              {key, source_for(source, key),
               "does not route to active peer dispatcher workspace " +
                   std::to_string(peer_dispatcher) + " for workload " +
                   std::to_string(workload.id)});
        }
      }
    } catch (const std::out_of_range&) {
      issues->push_back(
          {key, source_for(source, key),
           "peer does not define workload " + std::to_string(workload.id)});
    }
  }
}

void append_validation_issues(const ValidationResult& validation,
                              std::vector<ValidationIssue>* issues) {
  issues->insert(issues->end(), validation.issues().begin(),
                 validation.issues().end());
}

void append_pair_compatibility_issues(
    const AxioConfig& local, const AxioConfig& peer,
    std::vector<ValidationIssue>* issues) {
  const auto add_local_issue = [&](const std::string& key,
                                   const std::string& message) {
    issues->push_back({key, source_for(local, key), message});
  };
  if (local.schema_version != peer.schema_version) {
    add_local_issue("schema_version", "must match the peer schema version");
  }
  if (local.deployment.role == peer.deployment.role) {
    add_local_issue("deployment.role",
                    "endpoint pair must contain one client and one server");
  }
  if (local.network.backend != peer.network.backend) {
    add_local_issue("network.backend", "must match the peer backend");
  }
  if (local.network.backend == Backend::kRoce &&
      peer.network.backend == Backend::kRoce &&
      local.network.roce_transport != peer.network.roce_transport) {
    add_local_issue("network.roce_transport",
                    "must match the peer RoCE transport");
  }
  if (local.network.local_ip != peer.network.remote_ip) {
    add_local_issue("network.local_ip", "must equal peer network.remote_ip");
  }
  if (local.network.remote_ip != peer.network.local_ip) {
    add_local_issue("network.remote_ip", "must equal peer network.local_ip");
  }
  if (local.network.local_mac != peer.network.remote_mac) {
    add_local_issue("network.local_mac",
                    "must equal peer network.remote_mac");
  }
  if (local.network.remote_mac != peer.network.local_mac) {
    add_local_issue("network.remote_mac",
                    "must equal peer network.local_mac");
  }
}

}  // namespace

WorkspaceRole operator|(WorkspaceRole left, WorkspaceRole right) {
  return static_cast<WorkspaceRole>(static_cast<uint8_t>(left) |
                                    static_cast<uint8_t>(right));
}

bool has_role(WorkspaceRole roles, WorkspaceRole role) {
  return (static_cast<uint8_t>(roles) & static_cast<uint8_t>(role)) != 0;
}

TopologyError::TopologyError(std::string key, std::string message)
    : std::runtime_error(key + ": " + message),
      key_(std::move(key)),
      message_(std::move(message)) {}

ValidatedTopology ValidatedTopology::from_config(const AxioConfig& config) {
  ValidatedTopology topology;
  std::set<CpuCoreId> cpu_cores;

  for (size_t index = 0; index < config.workspaces.size(); ++index) {
    const WorkspaceConfig& workspace = config.workspaces[index];
    const WorkspaceId workspace_id(workspace.id);
    const CpuCoreId cpu_core(workspace.cpu_core);
    if (workspace.id >= kRuntimeWorkspaceLimit) {
      throw TopologyError(workspace_key(index, "id"),
                          "must be less than " +
                              std::to_string(kRuntimeWorkspaceLimit));
    }
    if (!topology.workspaces_
             .emplace(workspace_id,
                      ValidatedWorkspace{workspace_id, cpu_core})
             .second) {
      throw TopologyError(workspace_key(index, "id"),
                          "workspace ID must be unique");
    }
    if (!cpu_cores.insert(cpu_core).second) {
      throw TopologyError(workspace_key(index, "cpu_core"),
                          "CPU core must be assigned only once");
    }
    topology.active_workspace_ids_.push_back(workspace_id);
    topology.roles_.emplace(workspace_id, WorkspaceRole::kNone);
  }

  const std::set<WorkspaceId> application_resources = validate_resource_pool(
      config.tuning.resources.application_workspaces,
      "tuning.resources.application_workspaces", topology.workspaces_);
  const std::set<WorkspaceId> dispatcher_resources = validate_resource_pool(
      config.tuning.resources.dispatcher_workspaces,
      "tuning.resources.dispatcher_workspaces", topology.workspaces_);

  for (size_t workload_index = 0; workload_index < config.workloads.size();
       ++workload_index) {
    const WorkloadConfig& workload = config.workloads[workload_index];
    if (workload.id >= kRuntimeWorkloadIdLimit) {
      throw TopologyError(workload_key(workload_index, "id"),
                          "must be less than " +
                              std::to_string(kRuntimeWorkloadIdLimit));
    }
    if (topology.workloads_.find(workload.id) != topology.workloads_.end()) {
      throw TopologyError(workload_key(workload_index, "id"),
                          "workload ID must be unique");
    }
    if (workload.pipeline.empty()) {
      throw TopologyError(workload_key(workload_index, "pipeline"),
                          "must contain at least one stage");
    }
    std::set<PipelinePhase> phases;
    for (const PipelinePhase phase : workload.pipeline) {
      if (!is_valid_phase(phase)) {
        throw TopologyError(workload_key(workload_index, "pipeline"),
                            "contains an invalid pipeline stage");
      }
      if (!phases.insert(phase).second) {
        throw TopologyError(workload_key(workload_index, "pipeline"),
                            "pipeline stages must be unique");
      }
    }
    const bool has_application_stage =
        phases.count(PipelinePhase::kApplicationTx) != 0 ||
        phases.count(PipelinePhase::kApplicationRx) != 0;
    const bool has_dispatcher_stage =
        phases.count(PipelinePhase::kDispatcherTx) != 0 ||
        phases.count(PipelinePhase::kDispatcherRx) != 0;
    ValidatedWorkload validated_workload{workload.id, workload.pipeline, {},
                                         {}};
    std::set<uint32_t> remote_dispatchers;
    for (const uint32_t remote_dispatcher : workload.remote_dispatchers) {
      if (!remote_dispatchers.insert(remote_dispatcher).second) {
        throw TopologyError(
            workload_key(workload_index, "remote_dispatchers"),
            "must not contain duplicate dispatcher workspace IDs");
      }
      validated_workload.remote_dispatchers.emplace_back(remote_dispatcher);
    }

    for (size_t group_index = 0; group_index < workload.groups.size();
         ++group_index) {
      const WorkloadGroupConfig& group = workload.groups[group_index];
      if (!has_dispatcher_stage) {
        throw TopologyError(workload_key(workload_index, "pipeline"),
                            "must include a dispatcher stage when groups are "
                            "configured");
      }
      if (!group.applications.empty() && !has_application_stage) {
        throw TopologyError(workload_key(workload_index, "pipeline"),
                            "must include an application stage when a group "
                            "owns applications");
      }
      if (group.applications.empty()) {
        throw TopologyError(group_key(workload_index, group_index,
                                      "applications"),
                            "must contain at least one application workspace");
      }
      const WorkspaceId dispatcher(group.dispatcher);
      auto dispatcher_workspace = topology.workspaces_.find(dispatcher);
      if (dispatcher_workspace == topology.workspaces_.end()) {
        throw TopologyError(group_key(workload_index, group_index,
                                      "dispatcher"),
                            "references undefined workspace " +
                                std::to_string(group.dispatcher));
      }
      topology.roles_[dispatcher] =
          topology.roles_.at(dispatcher) | WorkspaceRole::kDispatcher;
      std::vector<uint32_t>& dispatcher_workloads =
          topology.dispatcher_workloads_[dispatcher];
      if (std::find(dispatcher_workloads.begin(), dispatcher_workloads.end(),
                    workload.id) == dispatcher_workloads.end()) {
        dispatcher_workloads.push_back(workload.id);
      }

      ValidatedGroup validated_group{dispatcher, {}};

      for (const uint32_t application_value : group.applications) {
        const WorkspaceId application(application_value);
        if (topology.workspaces_.find(application) ==
            topology.workspaces_.end()) {
          throw TopologyError(group_key(workload_index, group_index,
                                        "applications"),
                              "references undefined workspace " +
                                  std::to_string(application_value));
        }
        if (!topology.application_owners_
                 .emplace(application,
                          ApplicationOwner{workload.id, group_index,
                                           dispatcher})
                 .second) {
          throw TopologyError(group_key(workload_index, group_index,
                                        "applications"),
                              "application workspace " +
                                  std::to_string(application_value) +
                                  " belongs to more than one group");
        }
        topology.roles_[application] =
            topology.roles_.at(application) | WorkspaceRole::kApplication;
        validated_group.applications.push_back(application);
      }
      validated_workload.groups.push_back(std::move(validated_group));
    }
    if (!validated_workload.groups.empty() &&
        validated_workload.remote_dispatchers.empty()) {
      throw TopologyError(workload_key(workload_index, "remote_dispatchers"),
                          "must not be empty for an active workload");
    }
    topology.workloads_.emplace(workload.id, std::move(validated_workload));
    topology.active_workload_ids_.push_back(workload.id);
  }

  for (const auto& application : topology.application_owners_) {
    if (application_resources.count(application.first) == 0) {
      throw TopologyError("tuning.resources.application_workspaces",
                          "must include active application workspace " +
                              std::to_string(application.first.value()));
    }
  }
  for (const auto& dispatcher : topology.dispatcher_workloads_) {
    if (dispatcher_resources.count(dispatcher.first) == 0) {
      throw TopologyError("tuning.resources.dispatcher_workspaces",
                          "must include active dispatcher workspace " +
                              std::to_string(dispatcher.first.value()));
    }
  }

  if (topology.application_core_count() !=
      config.knobs.runtime.application_core_count) {
    throw TopologyError(
        "knobs.runtime.application_core_count",
        "does not match the " +
            std::to_string(topology.application_core_count()) +
            " application workspaces in topology");
  }
  if (topology.dispatcher_queue_count() !=
      config.knobs.runtime.dispatcher_queue_count) {
    throw TopologyError(
        "knobs.runtime.dispatcher_queue_count",
        "does not match the " +
            std::to_string(topology.dispatcher_queue_count()) +
            " dispatcher workspaces in topology");
  }

  topology.active_workspace_ids_.erase(
      std::remove_if(topology.active_workspace_ids_.begin(),
                     topology.active_workspace_ids_.end(),
                     [&](WorkspaceId id) {
                       return topology.roles_.at(id) == WorkspaceRole::kNone;
                     }),
      topology.active_workspace_ids_.end());

  return topology;
}

const ValidatedWorkspace& ValidatedTopology::workspace(WorkspaceId id) const {
  const auto workspace = this->workspaces_.find(id);
  if (workspace == this->workspaces_.end()) {
    throw std::out_of_range("unknown workspace " +
                            std::to_string(id.value()));
  }
  return workspace->second;
}

const ValidatedWorkload& ValidatedTopology::workload(uint32_t id) const {
  const auto workload = this->workloads_.find(id);
  if (workload == this->workloads_.end()) {
    throw std::out_of_range("unknown workload " + std::to_string(id));
  }
  return workload->second;
}

WorkspaceRole ValidatedTopology::roles(WorkspaceId id) const {
  const auto roles = this->roles_.find(id);
  return roles == this->roles_.end() ? WorkspaceRole::kNone : roles->second;
}

const ApplicationOwner& ValidatedTopology::application_owner(
    WorkspaceId id) const {
  const auto owner = this->application_owners_.find(id);
  if (owner == this->application_owners_.end()) {
    throw std::out_of_range("workspace is not an application: " +
                            std::to_string(id.value()));
  }
  return owner->second;
}

const std::vector<uint32_t>& ValidatedTopology::dispatcher_workloads(
    WorkspaceId id) const {
  const auto workloads = this->dispatcher_workloads_.find(id);
  if (workloads == this->dispatcher_workloads_.end()) {
    throw std::out_of_range("workspace is not a dispatcher: " +
                            std::to_string(id.value()));
  }
  return workloads->second;
}

bool ValidatedTopology::is_dispatcher_for_workload(
    WorkspaceId id, uint32_t workload_id) const {
  const auto workloads = this->dispatcher_workloads_.find(id);
  if (workloads == this->dispatcher_workloads_.end()) return false;
  return std::find(workloads->second.begin(), workloads->second.end(),
                   workload_id) != workloads->second.end();
}

void ValidatedTopology::validate_cpu_core_capacity(
    size_t available_core_count) const {
  for (const auto& [workspace_id, workspace] : this->workspaces_) {
    if (workspace.cpu_core.value() >= available_core_count) {
      throw TopologyError(
          "workspaces",
          "workspace " + std::to_string(workspace_id.value()) +
              " selects NUMA-local CPU core " +
              std::to_string(workspace.cpu_core.value()) + ", but only " +
              std::to_string(available_core_count) + " cores are available");
    }
  }
}

ValidationResult validate_config_pair(const AxioConfig& local,
                                      const AxioConfig& peer) {
  std::vector<ValidationIssue> issues;
  const ValidationResult local_validation = validate_config(local);
  const ValidationResult peer_validation = validate_config(peer);
  append_validation_issues(local_validation, &issues);
  append_validation_issues(peer_validation, &issues);
  if (!local_validation.ok() || !peer_validation.ok()) {
    return ValidationResult(std::move(issues));
  }

  const ValidatedTopology local_topology =
      ValidatedTopology::from_config(local);
  const ValidatedTopology peer_topology = ValidatedTopology::from_config(peer);
  append_pair_compatibility_issues(local, peer, &issues);
  append_pair_issues(local, local_topology, peer_topology, &issues);
  append_pair_issues(peer, peer_topology, local_topology, &issues);
  return ValidationResult(std::move(issues));
}

}  // namespace axio::config
