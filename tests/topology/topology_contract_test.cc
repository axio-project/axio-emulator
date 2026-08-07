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
  value.knobs.runtime.application_core_count = 2;
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
  value.tuning.max_iterations = 1;
  value.tuning.latency_slo_us = 100.0;
  value.tuning.sample_windows = 1;
  value.tuning.infrastructure_failure_limit = 1;
  value.deployment.topology.workspaces = {{0, 0}, {4, 4}, {5, 5}};
  value.deployment.topology.application_workspaces = {4, 5};
  value.deployment.topology.dispatcher_workspaces = {0};
  value.deployment.topology.workloads = {{
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

config::AxioConfig valid_peer_config() {
  config::AxioConfig peer = valid_config();
  peer.deployment.role = config::Role::kServer;
  peer.deployment.host = "server.example.net";
  std::swap(peer.network.local_ip, peer.network.remote_ip);
  std::swap(peer.network.local_mac, peer.network.remote_mac);
  return peer;
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
    expect(error.key() == "deployment.topology.workspaces",
           "CPU-capacity error must identify workspaces");
  }
}

void test_unique_workspace_ids_and_cpu_cores() {
  config::AxioConfig duplicate_id = valid_config();
  duplicate_id.deployment.topology.workspaces.push_back({4, 6});
  expect_topology_error(duplicate_id, "deployment.topology.workspaces");

  config::AxioConfig duplicate_core = valid_config();
  duplicate_core.deployment.topology.workspaces.push_back({6, 4});
  expect_topology_error(duplicate_core, "cpu_core");

  config::AxioConfig out_of_range_id = valid_config();
  out_of_range_id.deployment.topology.workspaces.push_back({config::kRuntimeWorkspaceLimit, 6});
  expect_topology_error(out_of_range_id, "deployment.topology.workspaces");
}

void test_pipeline_and_group_invariants() {
  config::AxioConfig empty_pipeline = valid_config();
  empty_pipeline.deployment.topology.workloads[0].pipeline.clear();
  expect_topology_error(empty_pipeline, "pipeline");

  config::AxioConfig duplicate_phase = valid_config();
  duplicate_phase.deployment.topology.workloads[0].pipeline.push_back(
      config::PipelinePhase::kApplicationTx);
  expect_topology_error(duplicate_phase, "pipeline");

  config::AxioConfig invalid_phase = valid_config();
  invalid_phase.deployment.topology.workloads[0].pipeline[0] =
      static_cast<config::PipelinePhase>(255);
  expect_topology_error(invalid_phase, "pipeline");

  config::AxioConfig duplicate_application = valid_config();
  duplicate_application.deployment.topology.workloads[0].groups.push_back({0, {4}});
  expect_topology_error(duplicate_application, "applications");

  config::AxioConfig cross_workload_application = valid_config();
  cross_workload_application.deployment.topology.workloads.push_back({
      2,
      {config::PipelinePhase::kDispatcherRx,
       config::PipelinePhase::kApplicationRx},
      {0},
      {{0, {4}}},
  });
  expect_topology_error(cross_workload_application, "applications");

  config::AxioConfig missing_workspace = valid_config();
  missing_workspace.deployment.topology.workloads[0].groups[0].applications.push_back(99);
  expect_topology_error(missing_workspace, "applications");

  config::AxioConfig missing_dispatcher = valid_config();
  missing_dispatcher.deployment.topology.workloads[0].groups[0].dispatcher = 99;
  expect_topology_error(missing_dispatcher, "dispatcher");

  config::AxioConfig duplicate_workload = valid_config();
  duplicate_workload.deployment.topology.workloads.push_back(duplicate_workload.deployment.topology.workloads[0]);
  expect_topology_error(duplicate_workload, "deployment.topology.workloads");

  config::AxioConfig reserved_workload_id = valid_config();
  reserved_workload_id.deployment.topology.workloads[0].id = config::kRuntimeWorkloadIdLimit;
  expect_topology_error(reserved_workload_id, "deployment.topology.workloads");

  config::AxioConfig dispatcher_without_stage = valid_config();
  dispatcher_without_stage.deployment.topology.workloads[0].pipeline.erase(
      std::remove(dispatcher_without_stage.deployment.topology.workloads[0].pipeline.begin(),
                  dispatcher_without_stage.deployment.topology.workloads[0].pipeline.end(),
                  config::PipelinePhase::kDispatcherTx),
      dispatcher_without_stage.deployment.topology.workloads[0].pipeline.end());
  dispatcher_without_stage.deployment.topology.workloads[0].pipeline.erase(
      std::remove(dispatcher_without_stage.deployment.topology.workloads[0].pipeline.begin(),
                  dispatcher_without_stage.deployment.topology.workloads[0].pipeline.end(),
                  config::PipelinePhase::kDispatcherRx),
      dispatcher_without_stage.deployment.topology.workloads[0].pipeline.end());
  expect_topology_error(dispatcher_without_stage, "pipeline");

  config::AxioConfig application_without_stage = valid_config();
  application_without_stage.deployment.topology.workloads[0].pipeline.erase(
      std::remove(application_without_stage.deployment.topology.workloads[0].pipeline.begin(),
                  application_without_stage.deployment.topology.workloads[0].pipeline.end(),
                  config::PipelinePhase::kApplicationTx),
      application_without_stage.deployment.topology.workloads[0].pipeline.end());
  application_without_stage.deployment.topology.workloads[0].pipeline.erase(
      std::remove(application_without_stage.deployment.topology.workloads[0].pipeline.begin(),
                  application_without_stage.deployment.topology.workloads[0].pipeline.end(),
                  config::PipelinePhase::kApplicationRx),
      application_without_stage.deployment.topology.workloads[0].pipeline.end());
  expect_topology_error(application_without_stage, "pipeline");

  config::AxioConfig empty_group = valid_config();
  empty_group.knobs.runtime.application_core_count = 1;
  empty_group.deployment.topology.workloads[0].groups[0].applications.clear();
  expect_topology_error(empty_group, "applications");

  config::AxioConfig empty_remote_pool = valid_config();
  empty_remote_pool.deployment.topology.workloads[0].remote_dispatchers.clear();
  expect_topology_error(empty_remote_pool, "remote_dispatchers");

  config::AxioConfig duplicate_remote = valid_config();
  duplicate_remote.deployment.topology.workloads[0].remote_dispatchers.push_back(0);
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
  value.deployment.topology.workspaces.push_back({6, 6});
  value.deployment.topology.application_workspaces.push_back(0);
  value.deployment.topology.application_workspaces.push_back(6);
  value.deployment.topology.workloads[0].groups[0].applications.push_back(0);
  value.deployment.topology.workloads.push_back({
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
  value.deployment.topology.workloads[0].remote_dispatchers = {7, 3, 5};
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
  value.deployment.topology.workspaces.push_back({6, 6});
  value.deployment.topology.application_workspaces.push_back(6);
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
  missing_application_resource.deployment.topology.application_workspaces = {4};
  expect_topology_error(missing_application_resource,
                        "deployment.topology.application_workspaces");

  config::AxioConfig missing_dispatcher_resource = valid_config();
  missing_dispatcher_resource.deployment.topology.dispatcher_workspaces.clear();
  expect_topology_error(missing_dispatcher_resource,
                        "deployment.topology.dispatcher_workspaces");
}

void test_pair_requires_peer_dispatcher() {
  const config::AxioConfig local = valid_config();
  config::AxioConfig peer = valid_peer_config();
  peer.deployment.topology.workloads[0].groups[0].dispatcher = 4;
  peer.deployment.topology.dispatcher_workspaces.push_back(4);

  const config::ValidationResult invalid =
      config::validate_config_pair(local, peer);
  expect(!invalid.ok(), "missing peer dispatcher ID must fail pair validation");
  expect(invalid.format().find("remote_dispatchers") != std::string::npos,
         "pair error must name remote_dispatchers");

  peer = valid_peer_config();
  expect(config::validate_config_pair(local, peer).ok(),
         "matching peer dispatcher IDs must validate");

  config::AxioConfig uncovered_peer = valid_peer_config();
  uncovered_peer.deployment.topology.workloads[0].groups[0].applications = {4};
  uncovered_peer.deployment.topology.workloads[0].groups.push_back({5, {5}});
  uncovered_peer.deployment.topology.dispatcher_workspaces.push_back(5);
  uncovered_peer.knobs.runtime.dispatcher_queue_count = 2;
  const config::ValidationResult uncovered =
      config::validate_config_pair(local, uncovered_peer);
  expect(!uncovered.ok(),
         "every active peer dispatcher must be covered by remote routes");
  expect(uncovered.format().find("does not route to active peer dispatcher") !=
             std::string::npos,
         "coverage failure must explain the unroutable peer dispatcher");

  config::AxioConfig same_role = valid_peer_config();
  same_role.deployment.role = config::Role::kClient;
  expect(!config::validate_config_pair(local, same_role).ok(),
         "a pair must contain one client and one server");

  config::AxioConfig backend_mismatch = valid_peer_config();
  backend_mismatch.network.backend = config::Backend::kRoce;
  backend_mismatch.knobs.build.mempool_handler =
      config::MempoolHandler::kHugeAlloc;
  expect(!config::validate_config_pair(local, backend_mismatch).ok(),
         "peer backends must match");

  config::AxioConfig address_mismatch = valid_peer_config();
  address_mismatch.network.remote_ip = "10.0.0.99";
  expect(!config::validate_config_pair(local, address_mismatch).ok(),
         "peer addresses must be reciprocal");

  config::AxioConfig attributed_local = valid_config();
  attributed_local.source_path = "local.toml";
  config::AxioConfig invalid_peer = valid_peer_config();
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

void test_pair_materialization_rebuilds_remote_routes() {
  config::AxioConfig local = valid_config();
  config::AxioConfig peer = valid_peer_config();
  local.deployment.topology.dispatcher_workspaces.push_back(5);
  peer.deployment.topology.dispatcher_workspaces.push_back(5);
  local.knobs.runtime.dispatcher_queue_count = 2;
  peer.knobs.runtime.dispatcher_queue_count = 2;

  config::materialize_topology_pair(&local, &peer);
  expect(local.deployment.topology.workloads[0].remote_dispatchers ==
             std::vector<uint32_t>({0, 5}),
         "local remote pool must cover materialized peer dispatchers");
  expect(peer.deployment.topology.workloads[0].remote_dispatchers ==
             std::vector<uint32_t>({0, 5}),
         "peer remote pool must cover materialized local dispatchers");
  expect(config::validate_config_pair(local, peer).ok(),
         "materialized endpoint pair must validate jointly");
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
    test_pair_materialization_rebuilds_remote_routes();
    std::cout << "Axio topology contract test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio topology contract test failed: " << error.what()
              << '\n';
    return 1;
  }
}
