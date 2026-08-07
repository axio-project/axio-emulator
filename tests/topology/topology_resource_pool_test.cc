#include "axio/config/topology.h"

#include <iostream>
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
  value.workspaces = {
      {0, 0}, {1, 1}, {2, 2}, {4, 4}, {5, 5}, {6, 6}, {7, 7}};
  value.tuning.resources.application_workspaces = {4, 5, 6, 7};
  value.tuning.resources.dispatcher_workspaces = {0, 1, 2};
  value.workloads = {
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
  for (const config::WorkloadConfig& workload : value.workloads) {
    for (const config::WorkloadGroupConfig& group : workload.groups) {
      if (group.dispatcher == dispatcher) return group;
    }
  }
  throw std::runtime_error("missing group for dispatcher " +
                           std::to_string(dispatcher));
}

void test_application_order_and_least_loaded_tie_break() {
  config::AxioConfig value = pool_config();
  value.knobs.runtime.application_core_count = 2;
  value.workloads[0].groups[0].applications = {4};
  value.workloads[0].groups.push_back({1, {5}});
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
  expect(value.workloads[0].groups.size() == 2,
         "new dispatcher must remain in the selected workload");
  expect(value.workloads[0].groups[1].dispatcher == 1,
         "new dispatcher must use configured resource order");
  expect(group_for(value, 0).applications == std::vector<uint32_t>({4, 6}),
         "dispatcher addition must rebalance applications deterministically");
  expect(group_for(value, 1).applications == std::vector<uint32_t>({5}),
         "dispatcher addition must balance applications within one");

  resources.add_dispatcher();
  expect(value.workloads[0].groups.back().dispatcher == 2,
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
  value.workloads[0].groups[0] = {0, {4}};
  value.workloads[0].groups.push_back({1, {5}});
  value.knobs.runtime.application_core_count = 1;
  value.knobs.runtime.dispatcher_queue_count = 1;
  config::materialize_topology(&value);

  expect(value.workloads[0].groups.size() == 1 &&
             value.workloads[0].groups[0].applications ==
                 std::vector<uint32_t>({4}),
         "remaining workload mapping must not be rewritten");
  expect(value.workloads[0].groups.size() == 1,
         "reverse C1/C2 removal must retire the empty dispatcher group");
}

void test_ambiguous_dispatcher_reuse_is_atomic() {
  config::AxioConfig value = pool_config();
  value.knobs.runtime.application_core_count = 4;
  value.workloads[0].groups.push_back({0, {7}});
  const config::AxioConfig unchanged = value;
  config::TopologyResourcePool resources(&value);
  try {
    resources.add_dispatcher();
    throw std::runtime_error("reused dispatcher mutation must fail");
  } catch (const config::TopologyError& error) {
    expect(error.key() == "tuning.resources.dispatcher_workspaces",
           "dispatcher-reuse error must name its resource pool");
  }
  expect(value.workloads[0].groups.size() ==
             unchanged.workloads[0].groups.size() &&
             value.workloads[0].groups.back().dispatcher ==
                 unchanged.workloads[0].groups.back().dispatcher &&
             value.workloads[0].groups.back().applications ==
                 unchanged.workloads[0].groups.back().applications,
         "ambiguous dispatcher failure must be atomic");
}

void test_multi_workload_scale_down_and_up() {
  config::AxioConfig value = pool_config();
  const std::vector<config::PipelinePhase> pipeline =
      value.workloads[0].pipeline;
  value.workloads[0].groups = {{0, {4}}};
  value.workloads.push_back({11, pipeline, {1}, {{1, {5}}}});
  value.workloads.push_back({12, pipeline, {2}, {{2, {6}}}});
  value.knobs.runtime.application_core_count = 3;
  value.knobs.runtime.dispatcher_queue_count = 3;

  value.knobs.runtime.application_core_count = 2;
  value.knobs.runtime.dispatcher_queue_count = 2;
  config::materialize_topology(&value);
  expect(value.workloads[0].groups[0].applications ==
             std::vector<uint32_t>({4}) &&
             value.workloads[1].groups[0].applications ==
                 std::vector<uint32_t>({5}) &&
             value.workloads[2].groups.empty(),
         "multi-workload scale-down must preserve surviving owners");

  value.knobs.runtime.application_core_count = 3;
  value.knobs.runtime.dispatcher_queue_count = 3;
  config::materialize_topology(&value);
  expect(value.workloads[0].groups.size() == 2 &&
             value.workloads[0].groups[0].applications ==
                 std::vector<uint32_t>({4}) &&
             value.workloads[0].groups[1].applications ==
                 std::vector<uint32_t>({6}) &&
             value.workloads[1].groups[0].applications ==
                 std::vector<uint32_t>({5}),
         "multi-workload scale-up must split a loaded workload without "
         "moving existing applications");
}

void test_combined_role_activation_and_deactivation() {
  config::AxioConfig application_role = pool_config();
  application_role.tuning.resources.application_workspaces = {4, 5, 6, 0, 7};
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
  dispatcher_role.tuning.resources.dispatcher_workspaces = {0, 4, 1, 2};
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
    expect(error.key() == "tuning.resources.application_workspaces",
           "pool exhaustion must name the application resource key");
  }
  expect(value.workloads.size() == unchanged.workloads.size() &&
             value.workspaces.size() == unchanged.workspaces.size() &&
             value.workspaces.front().id == unchanged.workspaces.front().id &&
             value.tuning.resources.application_workspaces ==
                 unchanged.tuning.resources.application_workspaces &&
             group_for(value, 0).applications ==
                 group_for(unchanged, 0).applications &&
             group_for(value, 1).applications ==
                 group_for(unchanged, 1).applications,
         "failed materialization must not partially mutate topology");
}

}  // namespace

int main() {
  try {
    test_application_order_and_least_loaded_tie_break();
    test_dispatcher_order_and_global_rebalance();
    test_removal_preserves_workload_ownership();
    test_ambiguous_dispatcher_reuse_is_atomic();
    test_multi_workload_scale_down_and_up();
    test_combined_role_activation_and_deactivation();
    test_materialize_counts_and_failure_atomicity();
    std::cout << "Axio topology resource-pool test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio topology resource-pool test failed: " << error.what()
              << '\n';
    return 1;
  }
}
