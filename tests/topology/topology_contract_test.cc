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
  expect(topology.application_owner(config::WorkspaceId(5)).dispatcher ==
             config::WorkspaceId(0),
         "application owner index must resolve its dispatcher");
}

void test_unique_workspace_ids_and_cpu_cores() {
  config::AxioConfig duplicate_id = valid_config();
  duplicate_id.workspaces.push_back({4, 6});
  expect_topology_error(duplicate_id, "workspaces");

  config::AxioConfig duplicate_core = valid_config();
  duplicate_core.workspaces.push_back({6, 4});
  expect_topology_error(duplicate_core, "cpu_core");
}

void test_pipeline_and_group_invariants() {
  config::AxioConfig empty_pipeline = valid_config();
  empty_pipeline.workloads[0].pipeline.clear();
  expect_topology_error(empty_pipeline, "pipeline");

  config::AxioConfig duplicate_phase = valid_config();
  duplicate_phase.workloads[0].pipeline.push_back(
      config::PipelinePhase::kApplicationTx);
  expect_topology_error(duplicate_phase, "pipeline");

  config::AxioConfig duplicate_application = valid_config();
  duplicate_application.workloads[0].groups.push_back({0, {4}});
  expect_topology_error(duplicate_application, "applications");

  config::AxioConfig missing_workspace = valid_config();
  missing_workspace.workloads[0].groups[0].applications.push_back(99);
  expect_topology_error(missing_workspace, "applications");
}

void test_dispatcher_reuse_and_combined_workspace() {
  config::AxioConfig value = valid_config();
  value.knobs.runtime.application_core_count = 3;
  value.workloads[0].groups[0].applications.push_back(0);
  value.workloads.push_back({
      2,
      {config::PipelinePhase::kDispatcherRx,
       config::PipelinePhase::kApplicationRx},
      {0},
      {{0, {}}},
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

void test_pair_requires_peer_dispatcher() {
  const config::AxioConfig local = valid_config();
  config::AxioConfig peer = valid_config();
  peer.workloads[0].groups[0].dispatcher = 4;

  const config::ValidationResult invalid =
      config::validate_config_pair(local, peer);
  expect(!invalid.ok(), "missing peer dispatcher ID must fail pair validation");
  expect(invalid.format().find("remote_dispatchers") != std::string::npos,
         "pair error must name remote_dispatchers");

  peer = valid_config();
  expect(config::validate_config_pair(local, peer).ok(),
         "matching peer dispatcher IDs must validate");
}

}  // namespace

int main() {
  try {
    test_indexes_and_derived_counts();
    test_unique_workspace_ids_and_cpu_cores();
    test_pipeline_and_group_invariants();
    test_dispatcher_reuse_and_combined_workspace();
    test_pair_requires_peer_dispatcher();
    std::cout << "Axio topology contract test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio topology contract test failed: " << error.what()
              << '\n';
    return 1;
  }
}
