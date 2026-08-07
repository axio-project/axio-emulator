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
          {config::PipelinePhase::kDispatcherRx,
           config::PipelinePhase::kApplicationRx},
          {0},
          {{0, {4, 5, 6}}},
      },
      {
          11,
          {config::PipelinePhase::kApplicationTx,
           config::PipelinePhase::kDispatcherTx},
          {1},
          {},
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
  value.workloads[1].groups.push_back({1, {6}});
  value.knobs.runtime.dispatcher_queue_count = 2;

  config::TopologyResourcePool resources(&value);
  resources.add_application();
  expect(group_for(value, 0).applications == std::vector<uint32_t>({4, 5}),
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
  expect(value.workloads[1].groups.size() == 1,
         "new dispatcher must select the workload with the fewest groups");
  expect(value.workloads[1].groups[0].dispatcher == 1,
         "new dispatcher must use configured resource order");
  expect(group_for(value, 0).applications == std::vector<uint32_t>({4, 6}),
         "dispatcher addition must rebalance applications deterministically");
  expect(group_for(value, 1).applications == std::vector<uint32_t>({5}),
         "dispatcher addition must balance applications within one");

  resources.add_dispatcher();
  expect(value.workloads[0].groups.back().dispatcher == 2,
         "workload ID must break equal group-count ties");
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
    test_materialize_counts_and_failure_atomicity();
    std::cout << "Axio topology resource-pool test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio topology resource-pool test failed: " << error.what()
              << '\n';
    return 1;
  }
}
