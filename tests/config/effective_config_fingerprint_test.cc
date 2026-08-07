#include "axio/config/effective_config.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace config = axio::config;

namespace {

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

config::AxioConfig base_config() {
  config::AxioConfig value;
  value.schema_version = 1;
  value.deployment.role = config::Role::kServer;
  value.deployment.numa_node = 0;
  value.deployment.host = "server.example.net";
  value.deployment.ssh_port = 22;
  value.deployment.ssh_user = "axio";
  value.deployment.workdir = "/opt/axio";
  value.deployment.use_sudo = true;
  value.network.backend = config::Backend::kDpdk;
  value.network.roce_transport = config::RoceTransport::kRc;
  value.network.physical_port = 0;
  value.network.rx_ring_entries = 2048;
  value.network.tx_ring_entries = 2048;
  value.network.local_ip = "10.0.0.1";
  value.network.remote_ip = "10.0.0.2";
  value.network.local_mac = "10:70:fd:00:00:01";
  value.network.remote_mac = "10:70:fd:00:00:02";
  value.network.device_pcie = "0000:98:00.0";
  value.network.device_name = "mlx5_0";
  value.handler.message_handler = config::MessageHandler::kThroughput;
  value.handler.packet_handler = config::PacketHandler::kEmpty;
  value.handler.request_payload_bytes = 982;
  value.handler.response_payload_bytes = 22;
  value.knobs.build.inflight_limit_enabled = true;
  value.knobs.build.inflight_messages = 1024;
  value.knobs.build.mtu = 2048;
  value.knobs.build.mempool_handler = config::MempoolHandler::kRingMpMc;
  value.knobs.runtime.application_core_count = 1;
  value.knobs.runtime.dispatcher_queue_count = 1;
  value.knobs.runtime.app_tx_batch_size = 32;
  value.knobs.runtime.app_rx_batch_size = 32;
  value.knobs.runtime.dispatcher_tx_batch_size = 32;
  value.knobs.runtime.dispatcher_rx_batch_size = 32;
  value.knobs.runtime.nic_tx_post_size = 32;
  value.knobs.runtime.nic_rx_post_size = 32;
  value.other.iterations = 30;
  value.other.window_seconds = 1;
  value.other.mempool_size = 8192;
  value.metrics.enabled = true;
  value.metrics.jsonl_path = "results/axio.jsonl";
  value.metrics.human_output = true;
  value.deployment.topology.application_workspaces = {4};
  value.deployment.topology.dispatcher_workspaces = {0};
  value.deployment.topology.workspaces = {{0, 0}, {4, 4}};
  config::WorkloadConfig workload;
  workload.id = 1;
  workload.pipeline = {config::PipelinePhase::kNicRx,
                       config::PipelinePhase::kDispatcherRx,
                       config::PipelinePhase::kApplicationRx};
  workload.remote_dispatchers = {0};
  workload.groups = {{0, {4}}};
  value.deployment.topology.workloads = {workload};
  return value;
}

void test_full_fingerprint_boundary() {
  const config::AxioConfig base = base_config();
  const std::string fingerprint = config::effective_config_fingerprint(base);
  expect(fingerprint.rfind("fnv1a64:", 0) == 0,
         "effective fingerprint must identify its hash format");

  config::AxioConfig relocated = base;
  relocated.metrics.jsonl_path = "/tmp/relocated.jsonl";
  relocated.source_path = "/tmp/source.toml";
  expect(config::effective_config_fingerprint(relocated) == fingerprint,
         "output and source paths must not alter datapath identity");

  config::AxioConfig runtime_knob = base;
  runtime_knob.knobs.runtime.app_rx_batch_size = 64;
  expect(config::effective_config_fingerprint(runtime_knob) != fingerprint,
         "runtime knobs must alter effective identity");

  config::AxioConfig deployment = base;
  deployment.deployment.host = "different.example.net";
  expect(config::effective_config_fingerprint(deployment) != fingerprint,
         "deployment values must alter effective identity");

  config::AxioConfig topology = base;
  topology.deployment.topology.workloads[0].groups[0].applications = {5};
  expect(config::effective_config_fingerprint(topology) != fingerprint,
         "topology mappings must alter effective identity");

  config::AxioConfig metrics_policy = base;
  metrics_policy.metrics.enabled = false;
  expect(config::effective_config_fingerprint(metrics_policy) != fingerprint,
         "metrics collection policy must alter effective identity");
}

}  // namespace

int main() {
  try {
    test_full_fingerprint_boundary();
    std::cout << "Axio effective config fingerprint test passed" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio effective config fingerprint test failed: "
              << error.what() << std::endl;
    return 1;
  }
}
