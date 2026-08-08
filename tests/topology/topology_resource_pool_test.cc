#include "axio/config/topology.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace config = axio::config;

namespace {

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

config::AxioConfig pool_config() {
  config::AxioConfig value;
  value.schema_version = 1;
  value.knobs.runtime.application_core_count = 3;
  value.knobs.runtime.dispatcher_queue_count = 1;
  value.deployment.topology.workspaces = {
      {0, 0}, {1, 1}, {2, 2}, {4, 4}, {5, 5}, {6, 6}, {7, 7}};
  value.deployment.topology.application_workspaces = {4, 5, 6, 7};
  value.deployment.topology.dispatcher_workspaces = {0, 1, 2};
  value.deployment.topology.workloads = {
      {
          10,
          {config::PipelinePhase::kApplicationTx,
           config::PipelinePhase::kDispatcherTx,
           config::PipelinePhase::kDispatcherRx,
           config::PipelinePhase::kApplicationRx},
          {0},
          {{0, {4, 5, 6}}},
      },
  };
  return value;
}

const config::WorkloadGroupConfig& group_for(
    const config::AxioConfig& value, uint32_t dispatcher) {
  for (const config::WorkloadConfig& workload : value.deployment.topology.workloads) {
    for (const config::WorkloadGroupConfig& group : workload.groups) {
      if (group.dispatcher == dispatcher) return group;
    }
  }
  throw std::runtime_error("missing group for dispatcher " +
                           std::to_string(dispatcher));
}

config::AxioConfig profile_config() {
  config::AxioConfig value = pool_config();
  value.deployment.role = config::Role::kClient;
  value.deployment.host = "client.example.net";
  value.deployment.ssh_user = "axio";
  value.deployment.workdir = "/opt/axio";
  value.network.backend = config::Backend::kDpdk;
  value.network.roce_transport = config::RoceTransport::kRc;
  value.network.rx_ring_entries = 2048;
  value.network.tx_ring_entries = 2048;
  value.network.local_ip = "10.0.0.1";
  value.network.remote_ip = "10.0.0.2";
  value.network.local_mac = "10:70:fd:00:00:01";
  value.network.remote_mac = "10:70:fd:00:00:02";
  value.network.device_pcie = "0000:98:00.0";
  value.network.device_name = "mlx5_0";
  value.handler.message_handler = config::MessageHandler::kThroughput;
  value.handler.request_payload_bytes = 982;
  value.handler.response_payload_bytes = 22;
  value.knobs.build.inflight_limit_enabled = true;
  value.knobs.build.inflight_messages = 1024;
  value.knobs.build.mtu = 2048;
  value.knobs.runtime.application_core_count = 1;
  value.knobs.runtime.dispatcher_queue_count = 1;
  value.knobs.runtime.app_tx_batch_size = 32;
  value.knobs.runtime.app_rx_batch_size = 32;
  value.knobs.runtime.dispatcher_tx_batch_size = 32;
  value.knobs.runtime.dispatcher_rx_batch_size = 32;
  value.knobs.runtime.nic_tx_post_size = 32;
  value.knobs.runtime.nic_rx_post_size = 32;
  value.other.iterations = 1;
  value.other.window_seconds = 1;
  value.other.mempool_size = 8192;
  value.metrics.jsonl_path = "results/axio.jsonl";
  value.tuning.emplace();
  value.tuning->max_iterations = 1;
  value.tuning->latency_slo_us = 100.0;
  value.tuning->sample_windows = 1;
  value.tuning->infrastructure_failure_limit = 1;
  value.deployment.topology.workspaces.clear();
  value.deployment.topology.application_workspaces.clear();
  value.deployment.topology.dispatcher_workspaces.clear();
  for (uint32_t id = 0; id < 16; ++id) {
    value.deployment.topology.workspaces.push_back({id, id});
    value.deployment.topology.application_workspaces.push_back(id);
    value.deployment.topology.dispatcher_workspaces.push_back(id);
  }
  value.deployment.topology.workloads = {{
      10,
      {config::PipelinePhase::kApplicationTx,
       config::PipelinePhase::kDispatcherTx,
       config::PipelinePhase::kNicTx, config::PipelinePhase::kNicRx,
       config::PipelinePhase::kDispatcherRx,
       config::PipelinePhase::kApplicationRx},
      {0},
      {{0, {0}}},
  }};
  return value;
}

config::AxioConfig profile_peer_config() {
  config::AxioConfig peer = profile_config();
  peer.deployment.role = config::Role::kServer;
  peer.deployment.host = "server.example.net";
  std::swap(peer.network.local_ip, peer.network.remote_ip);
  std::swap(peer.network.local_mac, peer.network.remote_mac);
  return peer;
}

config::AxioConfig peer_for_profile_config(config::AxioConfig peer) {
  peer.deployment.role = config::Role::kServer;
  peer.deployment.host = "server.example.net";
  std::swap(peer.network.local_ip, peer.network.remote_ip);
  std::swap(peer.network.local_mac, peer.network.remote_mac);
  return peer;
}

config::AxioConfig cross_paired_profile_config() {
  config::AxioConfig value = profile_config();
  value.knobs.runtime.application_core_count = 2;
  value.knobs.runtime.dispatcher_queue_count = 2;
  value.deployment.topology.application_workspaces = {1, 0, 3, 2};
  value.deployment.topology.dispatcher_workspaces = {1, 0};
  for (uint32_t id = 4; id < 16; ++id) {
    value.deployment.topology.application_workspaces.push_back(id);
  }
  for (uint32_t id = 2; id < 16; ++id) {
    value.deployment.topology.dispatcher_workspaces.push_back(id);
  }
  const std::vector<config::PipelinePhase> first_pipeline =
      value.deployment.topology.workloads[0].pipeline;
  const std::vector<config::PipelinePhase> second_pipeline = {
      config::PipelinePhase::kNicRx,
      config::PipelinePhase::kDispatcherRx,
      config::PipelinePhase::kApplicationRx,
  };
  value.deployment.topology.workloads = {
      {10, first_pipeline, {0}, {{0, {1}}}},
      {11, second_pipeline, {1}, {{1, {0}}}},
  };
  return value;
}

std::set<uint32_t> active_profile_applications(
    const config::AxioConfig& value) {
  std::set<uint32_t> active;
  for (const config::WorkloadConfig& workload :
       value.deployment.topology.workloads) {
    for (const config::WorkloadGroupConfig& group : workload.groups) {
      active.insert(group.applications.begin(), group.applications.end());
    }
  }
  return active;
}

std::set<uint32_t> active_profile_dispatchers(
    const config::AxioConfig& value) {
  std::set<uint32_t> active;
  for (const config::WorkloadConfig& workload :
       value.deployment.topology.workloads) {
    for (const config::WorkloadGroupConfig& group : workload.groups) {
      active.insert(group.dispatcher);
    }
  }
  return active;
}

bool same_workspace_definitions(const config::AxioConfig& left,
                                const config::AxioConfig& right) {
  if (left.deployment.topology.workspaces.size() !=
      right.deployment.topology.workspaces.size()) {
    return false;
  }
  for (size_t index = 0;
       index < left.deployment.topology.workspaces.size(); ++index) {
    const config::WorkspaceConfig& left_workspace =
        left.deployment.topology.workspaces[index];
    const config::WorkspaceConfig& right_workspace =
        right.deployment.topology.workspaces[index];
    if (left_workspace.id != right_workspace.id ||
        left_workspace.cpu_core != right_workspace.cpu_core) {
      return false;
    }
  }
  return true;
}

bool same_topology(const config::AxioConfig& left,
                   const config::AxioConfig& right) {
  if (left.deployment.topology.application_workspaces !=
          right.deployment.topology.application_workspaces ||
      left.deployment.topology.dispatcher_workspaces !=
          right.deployment.topology.dispatcher_workspaces ||
      !same_workspace_definitions(left, right) ||
      left.deployment.topology.workloads.size() !=
          right.deployment.topology.workloads.size()) {
    return false;
  }
  for (size_t workload_index = 0;
       workload_index < left.deployment.topology.workloads.size();
       ++workload_index) {
    const config::WorkloadConfig& left_workload =
        left.deployment.topology.workloads[workload_index];
    const config::WorkloadConfig& right_workload =
        right.deployment.topology.workloads[workload_index];
    if (left_workload.id != right_workload.id ||
        left_workload.pipeline != right_workload.pipeline ||
        left_workload.remote_dispatchers != right_workload.remote_dispatchers ||
        left_workload.groups.size() != right_workload.groups.size()) {
      return false;
    }
    for (size_t group_index = 0; group_index < left_workload.groups.size();
         ++group_index) {
      const config::WorkloadGroupConfig& left_group =
          left_workload.groups[group_index];
      const config::WorkloadGroupConfig& right_group =
          right_workload.groups[group_index];
      if (left_group.dispatcher != right_group.dispatcher ||
          left_group.applications != right_group.applications) {
        return false;
      }
    }
  }
  return true;
}

bool groups_match(
    const config::AxioConfig& value,
    const std::vector<std::pair<uint32_t, std::vector<uint32_t>>>& expected) {
  const std::vector<config::WorkloadGroupConfig>& groups =
      value.deployment.topology.workloads.at(0).groups;
  if (groups.size() != expected.size()) return false;
  for (size_t index = 0; index < groups.size(); ++index) {
    if (groups[index].dispatcher != expected[index].first ||
        groups[index].applications != expected[index].second) {
      return false;
    }
  }
  return true;
}

void test_application_order_and_least_loaded_tie_break() {
  config::AxioConfig value = pool_config();
  value.knobs.runtime.application_core_count = 2;
  value.deployment.topology.workloads[0].groups[0].applications = {4};
  value.deployment.topology.workloads[0].groups.push_back({1, {5}});
  value.knobs.runtime.dispatcher_queue_count = 2;

  config::TopologyResourcePool resources(&value);
  resources.add_application();
  expect(group_for(value, 0).applications == std::vector<uint32_t>({4, 6}),
         "first inactive application must use configured resource order and "
         "the lowest dispatcher ID on a load tie");

  resources.remove_application();
  expect(group_for(value, 1).applications == std::vector<uint32_t>({5}),
         "application removal must use reverse configured resource order");
  expect(value.knobs.runtime.application_core_count == 2,
         "application operations must keep C1 synchronized");
}

void test_dispatcher_order_and_global_rebalance() {
  config::AxioConfig value = pool_config();
  config::TopologyResourcePool resources(&value);

  resources.add_dispatcher();
  expect(value.deployment.topology.workloads[0].groups.size() == 2,
         "new dispatcher must remain in the selected workload");
  expect(value.deployment.topology.workloads[0].groups[1].dispatcher == 1,
         "new dispatcher must use configured resource order");
  expect(group_for(value, 0).applications == std::vector<uint32_t>({4, 6}),
         "dispatcher addition must rebalance applications deterministically");
  expect(group_for(value, 1).applications == std::vector<uint32_t>({5}),
         "dispatcher addition must balance applications within one");

  resources.add_dispatcher();
  expect(value.deployment.topology.workloads[0].groups.back().dispatcher == 2,
         "subsequent dispatchers must preserve workload ownership");
  expect(group_for(value, 0).applications == std::vector<uint32_t>({4}),
         "three-way rebalance must preserve configured application order");
  expect(group_for(value, 1).applications == std::vector<uint32_t>({5}),
         "three-way rebalance must preserve configured application order");
  expect(group_for(value, 2).applications == std::vector<uint32_t>({6}),
         "three-way rebalance must preserve configured application order");

  resources.remove_dispatcher();
  expect(group_for(value, 0).applications == std::vector<uint32_t>({4, 6}),
         "reverse dispatcher removal must rebalance remaining groups");
  expect(value.knobs.runtime.dispatcher_queue_count == 2,
         "dispatcher operations must keep C2 synchronized");
}

void test_removal_preserves_workload_ownership() {
  config::AxioConfig value = pool_config();
  value.knobs.runtime.application_core_count = 2;
  value.knobs.runtime.dispatcher_queue_count = 2;
  value.deployment.topology.workloads[0].groups[0] = {0, {4}};
  value.deployment.topology.workloads[0].groups.push_back({1, {5}});
  value.knobs.runtime.application_core_count = 1;
  value.knobs.runtime.dispatcher_queue_count = 1;
  config::materialize_topology(&value);

  expect(value.deployment.topology.workloads[0].groups.size() == 1 &&
             value.deployment.topology.workloads[0].groups[0].applications ==
                 std::vector<uint32_t>({4}),
         "remaining workload mapping must not be rewritten");
  expect(value.deployment.topology.workloads[0].groups.size() == 1,
         "reverse C1/C2 removal must retire the empty dispatcher group");
}

void test_dispatcher_reuse_is_split_deterministically() {
  config::AxioConfig value = pool_config();
  value.knobs.runtime.application_core_count = 4;
  value.deployment.topology.workloads[0].groups.push_back({0, {7}});
  config::TopologyResourcePool resources(&value);
  resources.add_dispatcher();
  expect(value.knobs.runtime.dispatcher_queue_count == 2 &&
             group_for(value, 0).applications ==
                 std::vector<uint32_t>({4, 6}) &&
             group_for(value, 1).applications ==
                 std::vector<uint32_t>({5, 7}),
         "adding a dispatcher must split a reused assignment deterministically");

  resources.remove_dispatcher();
  expect(value.knobs.runtime.dispatcher_queue_count == 1 &&
             value.deployment.topology.workloads[0].groups.size() == 1 &&
             group_for(value, 0).applications ==
                 std::vector<uint32_t>({4, 5, 6, 7}),
         "removing a dispatcher must merge its applications into a survivor");
}

void test_dispatcher_count_scales_independently() {
  config::AxioConfig value = pool_config();
  const std::vector<config::PipelinePhase> pipeline =
      value.deployment.topology.workloads[0].pipeline;
  value.deployment.topology.workspaces.push_back({3, 3});
  value.deployment.topology.dispatcher_workspaces = {0, 1, 2, 3};
  value.deployment.topology.workloads[0].groups = {{0, {4}}};
  value.deployment.topology.workloads.push_back({11, pipeline, {1}, {{1, {5}}}});
  value.deployment.topology.workloads.push_back({12, pipeline, {2}, {{2, {6}}}});
  value.deployment.topology.workloads.push_back({13, pipeline, {3}, {{3, {7}}}});
  value.knobs.runtime.application_core_count = 4;
  value.knobs.runtime.dispatcher_queue_count = 3;

  config::materialize_topology(&value);
  expect(config::ValidatedTopology::from_config(value)
                 .dispatcher_queue_count() == 3,
         "C2 must scale down without changing C1");
  for (size_t workload_index = 0; workload_index < value.deployment.topology.workloads.size();
       ++workload_index) {
    expect(value.deployment.topology.workloads[workload_index].groups[0].applications ==
               std::vector<uint32_t>(
                   {static_cast<uint32_t>(4 + workload_index)}),
           "C2 scale-down must preserve every application workload owner");
  }

  value.knobs.runtime.dispatcher_queue_count = 1;
  config::materialize_topology(&value);
  expect(config::ValidatedTopology::from_config(value)
                 .dispatcher_queue_count() == 1,
         "C2 must continue scaling down through dispatcher reuse");
  for (size_t workload_index = 0; workload_index < value.deployment.topology.workloads.size();
       ++workload_index) {
    expect(value.deployment.topology.workloads[workload_index].groups.size() == 1 &&
               value.deployment.topology.workloads[workload_index].groups[0].dispatcher == 0 &&
               value.deployment.topology.workloads[workload_index].groups[0].applications ==
                   std::vector<uint32_t>(
                       {static_cast<uint32_t>(4 + workload_index)}),
           "C2=1 must reuse one dispatcher without moving applications");
  }

  value.knobs.runtime.dispatcher_queue_count = 4;
  config::materialize_topology(&value);
  expect(config::ValidatedTopology::from_config(value)
                 .dispatcher_queue_count() == 4,
         "C2 must scale back up by splitting reused assignments");
  for (size_t workload_index = 0; workload_index < value.deployment.topology.workloads.size();
       ++workload_index) {
    expect(value.deployment.topology.workloads[workload_index].groups.size() == 1 &&
               value.deployment.topology.workloads[workload_index].groups[0].applications ==
                   std::vector<uint32_t>(
                       {static_cast<uint32_t>(4 + workload_index)}),
           "C2 scale-up must preserve every application workload owner");
  }
}

void test_multi_workload_scale_down_and_up() {
  config::AxioConfig value = pool_config();
  const std::vector<config::PipelinePhase> pipeline =
      value.deployment.topology.workloads[0].pipeline;
  value.deployment.topology.workloads[0].groups = {{0, {4}}};
  value.deployment.topology.workloads.push_back({11, pipeline, {1}, {{1, {5}}}});
  value.deployment.topology.workloads.push_back({12, pipeline, {2}, {{2, {6}}}});
  value.knobs.runtime.application_core_count = 3;
  value.knobs.runtime.dispatcher_queue_count = 3;

  value.knobs.runtime.application_core_count = 2;
  value.knobs.runtime.dispatcher_queue_count = 2;
  config::materialize_topology(&value);
  expect(value.deployment.topology.workloads[0].groups[0].applications ==
             std::vector<uint32_t>({4}) &&
             value.deployment.topology.workloads[1].groups[0].applications ==
                 std::vector<uint32_t>({5}) &&
             value.deployment.topology.workloads[2].groups.empty(),
         "multi-workload scale-down must preserve surviving owners");

  value.knobs.runtime.application_core_count = 3;
  value.knobs.runtime.dispatcher_queue_count = 3;
  config::materialize_topology(&value);
  expect(value.deployment.topology.workloads[0].groups.size() == 2 &&
             value.deployment.topology.workloads[0].groups[0].applications ==
                 std::vector<uint32_t>({4}) &&
             value.deployment.topology.workloads[0].groups[1].applications ==
                 std::vector<uint32_t>({6}) &&
             value.deployment.topology.workloads[1].groups[0].applications ==
                 std::vector<uint32_t>({5}),
         "multi-workload scale-up must split a loaded workload without "
         "moving existing applications");
}

void test_combined_role_activation_and_deactivation() {
  config::AxioConfig application_role = pool_config();
  application_role.deployment.topology.application_workspaces = {4, 5, 6, 0, 7};
  config::TopologyResourcePool application_resources(&application_role);
  application_resources.add_application();
  config::ValidatedTopology topology =
      config::ValidatedTopology::from_config(application_role);
  expect(config::has_role(topology.roles(config::WorkspaceId(0)),
                          config::WorkspaceRole::kApplication) &&
             config::has_role(topology.roles(config::WorkspaceId(0)),
                              config::WorkspaceRole::kDispatcher),
         "application activation must support an existing dispatcher");
  application_resources.remove_application();
  topology = config::ValidatedTopology::from_config(application_role);
  expect(!config::has_role(topology.roles(config::WorkspaceId(0)),
                           config::WorkspaceRole::kApplication) &&
             config::has_role(topology.roles(config::WorkspaceId(0)),
                              config::WorkspaceRole::kDispatcher),
         "application removal must retain the dispatcher role");

  config::AxioConfig dispatcher_role = pool_config();
  dispatcher_role.deployment.topology.dispatcher_workspaces = {0, 4, 1, 2};
  config::TopologyResourcePool dispatcher_resources(&dispatcher_role);
  dispatcher_resources.add_dispatcher();
  topology = config::ValidatedTopology::from_config(dispatcher_role);
  expect(config::has_role(topology.roles(config::WorkspaceId(4)),
                          config::WorkspaceRole::kApplication) &&
             config::has_role(topology.roles(config::WorkspaceId(4)),
                              config::WorkspaceRole::kDispatcher),
         "dispatcher activation must support an existing application");
  dispatcher_resources.remove_dispatcher();
  topology = config::ValidatedTopology::from_config(dispatcher_role);
  expect(config::has_role(topology.roles(config::WorkspaceId(4)),
                          config::WorkspaceRole::kApplication) &&
             !config::has_role(topology.roles(config::WorkspaceId(4)),
                               config::WorkspaceRole::kDispatcher),
         "dispatcher removal must retain the application role");
}

void test_materialize_counts_and_failure_atomicity() {
  config::AxioConfig value = pool_config();
  value.knobs.runtime.application_core_count = 4;
  value.knobs.runtime.dispatcher_queue_count = 2;
  config::materialize_topology(&value);

  const config::ValidatedTopology topology =
      config::ValidatedTopology::from_config(value);
  expect(topology.application_core_count() == 4,
         "materialization must make explicit topology match C1");
  expect(topology.dispatcher_queue_count() == 2,
         "materialization must make explicit topology match C2");
  expect(group_for(value, 0).applications == std::vector<uint32_t>({4, 6}),
         "materialization must produce the deterministic golden layout");
  expect(group_for(value, 1).applications == std::vector<uint32_t>({5, 7}),
         "materialization must produce the deterministic golden layout");

  const config::AxioConfig unchanged = value;
  value.knobs.runtime.application_core_count = 5;
  try {
    config::materialize_topology(&value);
    throw std::runtime_error("exhausted application pool must fail");
  } catch (const config::TopologyError& error) {
    expect(error.key() == "deployment.topology.application_workspaces",
           "pool exhaustion must name the application resource key");
  }
  expect(value.deployment.topology.workloads.size() == unchanged.deployment.topology.workloads.size() &&
             value.deployment.topology.workspaces.size() == unchanged.deployment.topology.workspaces.size() &&
             value.deployment.topology.workspaces.front().id == unchanged.deployment.topology.workspaces.front().id &&
             value.deployment.topology.application_workspaces ==
                 unchanged.deployment.topology.application_workspaces &&
             group_for(value, 0).applications ==
                 group_for(unchanged, 0).applications &&
             group_for(value, 1).applications ==
                 group_for(unchanged, 1).applications,
         "failed materialization must not partially mutate topology");
}

void test_explicit_topology_profiles() {
  struct Case {
    config::TopologySearchProfile profile;
    uint32_t applications;
    uint32_t dispatchers;
    std::set<uint32_t> expected_applications;
    std::set<uint32_t> expected_dispatchers;
    size_t fanout;
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> expected_groups;
  };
  const std::vector<Case> cases = {
      {config::TopologySearchProfile::kColocatedOneToOne, 8, 8,
       {0, 1, 2, 3, 4, 5, 6, 7}, {0, 1, 2, 3, 4, 5, 6, 7}, 1,
       {{0, {0}}, {1, {1}}, {2, {2}}, {3, {3}},
        {4, {4}}, {5, {5}}, {6, {6}}, {7, {7}}}},
      {config::TopologySearchProfile::kSplitOneToOne, 8, 8,
       {0, 1, 2, 3, 4, 5, 6, 7}, {8, 9, 10, 11, 12, 13, 14, 15}, 1,
       {{8, {0}}, {9, {1}}, {10, {2}}, {11, {3}},
        {12, {4}}, {13, {5}}, {14, {6}}, {15, {7}}}},
      {config::TopologySearchProfile::kColocatedFanout, 16, 8,
       {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
       {0, 1, 2, 3, 4, 5, 6, 7}, 2,
       {{0, {0, 8}}, {1, {1, 9}}, {2, {2, 10}}, {3, {3, 11}},
        {4, {4, 12}}, {5, {5, 13}}, {6, {6, 14}}, {7, {7, 15}}}},
  };

  for (const Case& test_case : cases) {
    config::AxioConfig target = profile_config();
    config::AxioConfig peer = profile_peer_config();
    const config::AxioConfig target_source = target;
    const config::AxioConfig peer_source = peer;
    target.knobs.runtime.application_core_count = test_case.applications;
    target.knobs.runtime.dispatcher_queue_count = test_case.dispatchers;
    config::materialize_target_topology_profile_pair(&target, &peer,
                                                     test_case.profile);

    expect(active_profile_applications(target) ==
               test_case.expected_applications,
           "profile must select the exact ordered application role set");
    expect(active_profile_dispatchers(target) ==
               test_case.expected_dispatchers,
           "profile must select the exact ordered dispatcher role set");
    expect(groups_match(target, test_case.expected_groups),
           "profile must produce the exact ordered group layout");
    for (const config::WorkloadConfig& workload :
         target.deployment.topology.workloads) {
      expect(workload.pipeline ==
                 target_source.deployment.topology.workloads[0].pipeline,
             "profile must preserve pipeline order");
      for (const config::WorkloadGroupConfig& group : workload.groups) {
        expect(group.applications.size() == test_case.fanout,
               "profile must balance every dispatcher group");
      }
    }
    expect(same_workspace_definitions(target, target_source),
           "profile must preserve physical workspace definitions");
    expect(peer.knobs.runtime.application_core_count ==
               peer_source.knobs.runtime.application_core_count &&
               peer.knobs.runtime.dispatcher_queue_count ==
                   peer_source.knobs.runtime.dispatcher_queue_count &&
               peer.deployment.topology.workloads[0].groups[0].dispatcher ==
                   peer_source.deployment.topology.workloads[0]
                       .groups[0]
                       .dispatcher &&
               peer.deployment.topology.workloads[0]
                       .groups[0]
                       .applications ==
                   peer_source.deployment.topology.workloads[0]
                       .groups[0]
                       .applications &&
               peer.deployment.topology.workloads[0].pipeline ==
                   peer_source.deployment.topology.workloads[0].pipeline &&
               same_workspace_definitions(peer, peer_source),
           "target profile must preserve peer non-route fields");
    expect(target.deployment.topology.workloads[0].remote_dispatchers ==
               std::vector<uint32_t>({0}) &&
               std::set<uint32_t>(
                   peer.deployment.topology.workloads[0]
                       .remote_dispatchers.begin(),
                   peer.deployment.topology.workloads[0]
                       .remote_dispatchers.end()) ==
                   test_case.expected_dispatchers,
           "profile must rebuild reciprocal peer routes");
    expect(config::validate_config_pair(target, peer).ok(),
           "materialized profile pair must validate");
  }
}

void test_explicit_topology_profile_preserves_multi_workload_ownership() {
  config::AxioConfig target = profile_config();
  const std::vector<config::PipelinePhase> first_pipeline =
      target.deployment.topology.workloads[0].pipeline;
  const std::vector<config::PipelinePhase> second_pipeline = {
      config::PipelinePhase::kNicRx,
      config::PipelinePhase::kDispatcherRx,
      config::PipelinePhase::kApplicationRx,
  };
  target.knobs.runtime.application_core_count = 2;
  target.knobs.runtime.dispatcher_queue_count = 2;
  target.deployment.topology.workloads = {
      {10, first_pipeline, {0}, {{0, {0}}}},
      {11, second_pipeline, {1}, {{1, {1}}}},
  };
  config::AxioConfig peer = peer_for_profile_config(target);
  target.knobs.runtime.application_core_count = 4;
  target.knobs.runtime.dispatcher_queue_count = 2;

  config::materialize_target_topology_profile_pair(
      &target, &peer, config::TopologySearchProfile::kColocatedFanout);

  expect(target.deployment.topology.workloads[0].id == 10 &&
             target.deployment.topology.workloads[0].pipeline == first_pipeline &&
             target.deployment.topology.workloads[0].groups.size() == 1 &&
             target.deployment.topology.workloads[0].groups[0].dispatcher == 0 &&
             target.deployment.topology.workloads[0].groups[0].applications ==
                 std::vector<uint32_t>({0, 2}),
         "profile must preserve the first workload owner and pipeline");
  expect(target.deployment.topology.workloads[1].id == 11 &&
             target.deployment.topology.workloads[1].pipeline ==
                 second_pipeline &&
             target.deployment.topology.workloads[1].groups.size() == 1 &&
             target.deployment.topology.workloads[1].groups[0].dispatcher == 1 &&
             target.deployment.topology.workloads[1].groups[0].applications ==
                 std::vector<uint32_t>({1, 3}),
         "profile must preserve the second workload owner and pipeline");
  expect(config::validate_config_pair(target, peer).ok(),
         "multi-workload profile pair must validate");
}

void test_colocated_profiles_canonicalize_cross_paired_groups() {
  const config::AxioConfig source = cross_paired_profile_config();
  const config::AxioConfig peer_source = peer_for_profile_config(source);
  struct Case {
    config::TopologySearchProfile profile;
    uint32_t applications;
    std::vector<uint32_t> first_group_applications;
    std::vector<uint32_t> second_group_applications;
  };
  const std::vector<Case> cases = {
      {config::TopologySearchProfile::kColocatedOneToOne, 2, {0}, {1}},
      {config::TopologySearchProfile::kColocatedFanout, 4, {0, 2}, {1, 3}},
  };

  for (const Case& test_case : cases) {
    config::AxioConfig target = source;
    config::AxioConfig peer = peer_source;
    target.knobs.runtime.application_core_count = test_case.applications;
    config::materialize_target_topology_profile_pair(&target, &peer,
                                                     test_case.profile);

    expect(target.deployment.topology.workloads[0].id == 10 &&
               target.deployment.topology.workloads[0].groups.size() == 1 &&
               target.deployment.topology.workloads[0].groups[0].dispatcher ==
                   0 &&
               target.deployment.topology.workloads[0]
                       .groups[0]
                       .applications == test_case.first_group_applications,
           "colocated profile must canonicalize the first logical group");
    expect(target.deployment.topology.workloads[1].id == 11 &&
               target.deployment.topology.workloads[1].groups.size() == 1 &&
               target.deployment.topology.workloads[1].groups[0].dispatcher ==
                   1 &&
               target.deployment.topology.workloads[1]
                       .groups[0]
                       .applications == test_case.second_group_applications,
           "colocated profile must canonicalize the second logical group");
    for (const config::WorkloadConfig& workload :
         target.deployment.topology.workloads) {
      expect(std::find(workload.groups[0].applications.begin(),
                       workload.groups[0].applications.end(),
                       workload.groups[0].dispatcher) !=
                 workload.groups[0].applications.end(),
             "every colocated group must contain its dispatcher workspace");
    }
    expect(config::validate_config_pair(target, peer).ok(),
           "canonical cross-paired profile must validate");
  }
}

void test_explicit_topology_profile_failures_are_atomic() {
  struct FailureCase {
    config::TopologySearchProfile profile;
    uint32_t applications;
    uint32_t dispatchers;
  };
  const std::vector<FailureCase> cases = {
      {config::TopologySearchProfile::kSplitOneToOne, 9, 9},
      {config::TopologySearchProfile::kColocatedFanout, 15, 8},
  };
  for (const FailureCase& test_case : cases) {
    config::AxioConfig target = profile_config();
    config::AxioConfig peer = profile_peer_config();
    target.knobs.runtime.application_core_count = test_case.applications;
    target.knobs.runtime.dispatcher_queue_count = test_case.dispatchers;
    const config::AxioConfig target_source = target;
    const config::AxioConfig peer_source = peer;
    try {
      config::materialize_target_topology_profile_pair(&target, &peer,
                                                       test_case.profile);
      throw std::runtime_error("invalid topology profile must fail");
    } catch (const config::TopologyError&) {
    }
    expect(target.knobs.runtime.application_core_count ==
               target_source.knobs.runtime.application_core_count &&
               target.knobs.runtime.dispatcher_queue_count ==
                   target_source.knobs.runtime.dispatcher_queue_count &&
               peer.knobs.runtime.application_core_count ==
                   peer_source.knobs.runtime.application_core_count &&
               peer.knobs.runtime.dispatcher_queue_count ==
                   peer_source.knobs.runtime.dispatcher_queue_count &&
               same_topology(target, target_source) &&
               same_topology(peer, peer_source),
           "failed topology profile must not partially mutate either endpoint");
  }
}

}  // namespace

int main() {
  try {
    test_application_order_and_least_loaded_tie_break();
    test_dispatcher_order_and_global_rebalance();
    test_removal_preserves_workload_ownership();
    test_dispatcher_reuse_is_split_deterministically();
    test_dispatcher_count_scales_independently();
    test_multi_workload_scale_down_and_up();
    test_combined_role_activation_and_deactivation();
    test_materialize_counts_and_failure_atomicity();
    test_explicit_topology_profiles();
    test_explicit_topology_profile_preserves_multi_workload_ownership();
    test_colocated_profiles_canonicalize_cross_paired_groups();
    test_explicit_topology_profile_failures_are_atomic();
    std::cout << "Axio topology resource-pool test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio topology resource-pool test failed: " << error.what()
              << '\n';
    return 1;
  }
}
