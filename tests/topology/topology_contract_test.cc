#include "axio/config/topology.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace config = axio::config;

namespace {

void expect(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

config::AxioConfig valid_config() {
  config::AxioConfig value;
  value.schema_version = 1;
  value.knobs.runtime.application_core_count = 2;
  value.knobs.runtime.dispatcher_queue_count = 1;
  value.workspaces = {{0, 0}, {4, 4}, {5, 5}};
  value.tuning.resources.application_workspaces = {4, 5};
  value.tuning.resources.dispatcher_workspaces = {0};
  value.workloads = {{
      1,
      {config::PipelinePhase::kApplicationTx,
       config::PipelinePhase::kDispatcherTx, config::PipelinePhase::kNicTx,
       config::PipelinePhase::kNicRx,
       config::PipelinePhase::kDispatcherRx,
       config::PipelinePhase::kApplicationRx},
      {0},
      {{0, {4, 5}}},
  }};
  return value;
}

void expect_topology_error(const config::AxioConfig& value,
                           const std::string& key) {
  try {
    static_cast<void>(config::ValidatedTopology::from_config(value));
  } catch (const config::TopologyError& error) {
    expect(error.key().find(key) != std::string::npos,
           "topology error must name " + key + ": " + error.what());
    return;
  }
  throw std::runtime_error("expected topology error for " + key);
}

void test_indexes_and_derived_counts() {
  const config::ValidatedTopology topology =
      config::ValidatedTopology::from_config(valid_config());

  expect(topology.active_workspace_ids() ==
             std::vector<config::WorkspaceId>({config::WorkspaceId(0),
                                               config::WorkspaceId(4),
                                               config::WorkspaceId(5)}),
         "active workspaces must preserve TOML order");
  expect(topology.workspace(config::WorkspaceId(4)).cpu_core ==
             config::CpuCoreId(4),
         "workspace and CPU identifiers must remain distinct");
  expect(topology.application_core_count() == 2,
         "application count must be derived from the topology");
  expect(topology.dispatcher_queue_count() == 1,
         "dispatcher count must be derived from the topology");
  expect(topology.workload(1).groups.at(0).dispatcher ==
             config::WorkspaceId(0),
         "validated groups must expose typed dispatcher IDs");
  expect(topology.workload(1).groups.at(0).applications.at(1) ==
             config::WorkspaceId(5),
         "validated groups must expose typed application IDs");
  expect(topology.application_owner(config::WorkspaceId(5)).dispatcher ==
             config::WorkspaceId(0),
         "application owner index must resolve its dispatcher");
  topology.validate_cpu_core_capacity(6);
  try {
    topology.validate_cpu_core_capacity(5);
    throw std::runtime_error("out-of-range NUMA-local CPU core must fail");
  } catch (const config::TopologyError& error) {
    expect(error.key() == "workspaces",
           "CPU-capacity error must identify workspaces");
  }
}

void test_unique_workspace_ids_and_cpu_cores() {
  config::AxioConfig duplicate_id = valid_config();
  duplicate_id.workspaces.push_back({4, 6});
  expect_topology_error(duplicate_id, "workspaces");

  config::AxioConfig duplicate_core = valid_config();
  duplicate_core.workspaces.push_back({6, 4});
  expect_topology_error(duplicate_core, "cpu_core");

  config::AxioConfig out_of_range_id = valid_config();
  out_of_range_id.workspaces.push_back({config::kRuntimeWorkspaceLimit, 6});
  expect_topology_error(out_of_range_id, "workspaces");
}

void test_pipeline_and_group_invariants() {
  config::AxioConfig empty_pipeline = valid_config();
  empty_pipeline.workloads[0].pipeline.clear();
  expect_topology_error(empty_pipeline, "pipeline");

  config::AxioConfig duplicate_phase = valid_config();
  duplicate_phase.workloads[0].pipeline.push_back(
      config::PipelinePhase::kApplicationTx);
  expect_topology_error(duplicate_phase, "pipeline");

  config::AxioConfig invalid_phase = valid_config();
  invalid_phase.workloads[0].pipeline[0] =
      static_cast<config::PipelinePhase>(255);
  expect_topology_error(invalid_phase, "pipeline");

  config::AxioConfig duplicate_application = valid_config();
  duplicate_application.workloads[0].groups.push_back({0, {4}});
  expect_topology_error(duplicate_application, "applications");

  config::AxioConfig cross_workload_application = valid_config();
  cross_workload_application.workloads.push_back({
      2,
      {config::PipelinePhase::kDispatcherRx,
       config::PipelinePhase::kApplicationRx},
      {0},
      {{0, {4}}},
  });
  expect_topology_error(cross_workload_application, "applications");

  config::AxioConfig missing_workspace = valid_config();
  missing_workspace.workloads[0].groups[0].applications.push_back(99);
  expect_topology_error(missing_workspace, "applications");

  config::AxioConfig missing_dispatcher = valid_config();
  missing_dispatcher.workloads[0].groups[0].dispatcher = 99;
  expect_topology_error(missing_dispatcher, "dispatcher");

  config::AxioConfig duplicate_workload = valid_config();
  duplicate_workload.workloads.push_back(duplicate_workload.workloads[0]);
  expect_topology_error(duplicate_workload, "workloads");

  config::AxioConfig reserved_workload_id = valid_config();
  reserved_workload_id.workloads[0].id = config::kRuntimeWorkloadIdLimit;
  expect_topology_error(reserved_workload_id, "workloads");

  config::AxioConfig dispatcher_without_stage = valid_config();
  dispatcher_without_stage.workloads[0].pipeline.erase(
      std::remove(dispatcher_without_stage.workloads[0].pipeline.begin(),
                  dispatcher_without_stage.workloads[0].pipeline.end(),
                  config::PipelinePhase::kDispatcherTx),
      dispatcher_without_stage.workloads[0].pipeline.end());
  dispatcher_without_stage.workloads[0].pipeline.erase(
      std::remove(dispatcher_without_stage.workloads[0].pipeline.begin(),
                  dispatcher_without_stage.workloads[0].pipeline.end(),
                  config::PipelinePhase::kDispatcherRx),
      dispatcher_without_stage.workloads[0].pipeline.end());
  expect_topology_error(dispatcher_without_stage, "pipeline");

  config::AxioConfig application_without_stage = valid_config();
  application_without_stage.workloads[0].pipeline.erase(
      std::remove(application_without_stage.workloads[0].pipeline.begin(),
                  application_without_stage.workloads[0].pipeline.end(),
                  config::PipelinePhase::kApplicationTx),
      application_without_stage.workloads[0].pipeline.end());
  application_without_stage.workloads[0].pipeline.erase(
      std::remove(application_without_stage.workloads[0].pipeline.begin(),
                  application_without_stage.workloads[0].pipeline.end(),
                  config::PipelinePhase::kApplicationRx),
      application_without_stage.workloads[0].pipeline.end());
  expect_topology_error(application_without_stage, "pipeline");

  config::AxioConfig empty_group = valid_config();
  empty_group.knobs.runtime.application_core_count = 1;
  empty_group.workloads[0].groups[0].applications.clear();
  expect_topology_error(empty_group, "applications");

  config::AxioConfig empty_remote_pool = valid_config();
  empty_remote_pool.workloads[0].remote_dispatchers.clear();
  expect_topology_error(empty_remote_pool, "remote_dispatchers");

  config::AxioConfig duplicate_remote = valid_config();
  duplicate_remote.workloads[0].remote_dispatchers.push_back(0);
  expect_topology_error(duplicate_remote, "remote_dispatchers");
}

void test_configured_counts_match_topology() {
  config::AxioConfig application_mismatch = valid_config();
  application_mismatch.knobs.runtime.application_core_count = 1;
  expect_topology_error(application_mismatch,
                        "knobs.runtime.application_core_count");

  config::AxioConfig dispatcher_mismatch = valid_config();
  dispatcher_mismatch.knobs.runtime.dispatcher_queue_count = 2;
  expect_topology_error(dispatcher_mismatch,
                        "knobs.runtime.dispatcher_queue_count");
}

void test_dispatcher_reuse_and_combined_workspace() {
  config::AxioConfig value = valid_config();
  value.knobs.runtime.application_core_count = 4;
  value.workspaces.push_back({6, 6});
  value.tuning.resources.application_workspaces.push_back(0);
  value.tuning.resources.application_workspaces.push_back(6);
  value.workloads[0].groups[0].applications.push_back(0);
  value.workloads.push_back({
      2,
      {config::PipelinePhase::kDispatcherRx,
       config::PipelinePhase::kApplicationRx},
      {0},
      {{0, {6}}},
  });

  const config::ValidatedTopology topology =
      config::ValidatedTopology::from_config(value);
  expect(config::has_role(topology.roles(config::WorkspaceId(0)),
                          config::WorkspaceRole::kDispatcher),
         "dispatcher role must be indexed");
  expect(config::has_role(topology.roles(config::WorkspaceId(0)),
                          config::WorkspaceRole::kApplication),
         "one workspace must be able to serve multiple stage roles");
  expect(topology.dispatcher_workloads(config::WorkspaceId(0)).size() == 2,
         "a dispatcher must be reusable across workloads");
}

void test_remote_dispatcher_order_is_preserved() {
  config::AxioConfig value = valid_config();
  value.workloads[0].remote_dispatchers = {7, 3, 5};
  const config::ValidatedTopology topology =
      config::ValidatedTopology::from_config(value);
  expect(topology.workload(1).remote_dispatchers ==
             std::vector<config::WorkspaceId>({config::WorkspaceId(7),
                                               config::WorkspaceId(3),
                                               config::WorkspaceId(5)}),
         "remote dispatcher order must be preserved for round-robin");
}

void test_resource_pool_and_active_workspace_boundaries() {
  config::AxioConfig value = valid_config();
  value.workspaces.push_back({6, 6});
  value.tuning.resources.application_workspaces.push_back(6);
  const config::ValidatedTopology topology =
      config::ValidatedTopology::from_config(value);
  expect(topology.active_workspace_ids().size() == 3,
         "inactive tuning candidates must not launch a workspace");
  expect(std::find(topology.active_workspace_ids().begin(),
                   topology.active_workspace_ids().end(),
                   config::WorkspaceId(6)) ==
             topology.active_workspace_ids().end(),
         "inactive tuning candidate was treated as active");

  config::AxioConfig missing_application_resource = valid_config();
  missing_application_resource.tuning.resources.application_workspaces = {4};
  expect_topology_error(missing_application_resource,
                        "tuning.resources.application_workspaces");

  config::AxioConfig missing_dispatcher_resource = valid_config();
  missing_dispatcher_resource.tuning.resources.dispatcher_workspaces.clear();
  expect_topology_error(missing_dispatcher_resource,
                        "tuning.resources.dispatcher_workspaces");
}

void test_pair_requires_peer_dispatcher() {
  const config::AxioConfig local = valid_config();
  config::AxioConfig peer = valid_config();
  peer.workloads[0].groups[0].dispatcher = 4;
  peer.tuning.resources.dispatcher_workspaces.push_back(4);

  const config::ValidationResult invalid =
      config::validate_config_pair(local, peer);
  expect(!invalid.ok(), "missing peer dispatcher ID must fail pair validation");
  expect(invalid.format().find("remote_dispatchers") != std::string::npos,
         "pair error must name remote_dispatchers");

  peer = valid_config();
  expect(config::validate_config_pair(local, peer).ok(),
         "matching peer dispatcher IDs must validate");

  config::AxioConfig attributed_local = valid_config();
  attributed_local.source_path = "local.toml";
  config::AxioConfig invalid_peer = valid_config();
  invalid_peer.source_path = "peer.toml";
  invalid_peer.knobs.runtime.dispatcher_queue_count = 2;
  const std::string diagnostic =
      config::validate_config_pair(attributed_local, invalid_peer).format();
  expect(diagnostic.find("peer.toml") != std::string::npos,
         "peer topology error must retain the peer source path");
  expect(diagnostic.find("local.toml") == std::string::npos,
         "peer topology error must not be attributed to the local config");
  expect(diagnostic.find(
             "dispatcher_queue_count: knobs.runtime.dispatcher_queue_count") ==
             std::string::npos,
         "pair diagnostics must not duplicate the failing key");
}

}  // namespace

int main() {
  try {
    test_indexes_and_derived_counts();
    test_unique_workspace_ids_and_cpu_cores();
    test_pipeline_and_group_invariants();
    test_configured_counts_match_topology();
    test_dispatcher_reuse_and_combined_workspace();
    test_remote_dispatcher_order_is_preserved();
    test_resource_pool_and_active_workspace_boundaries();
    test_pair_requires_peer_dispatcher();
    std::cout << "Axio topology contract test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio topology contract test failed: " << error.what()
              << '\n';
    return 1;
  }
}
