#include "axio/config/config_loader.h"
#include "axio/config/config_validator.h"
#include "config.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

void expect(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void test_fingerprint_boundary(const axio::config::AxioConfig& loaded) {
  const axio::BuildFingerprintComparison matching =
      axio::compare_build_fingerprint(loaded);
  expect(matching.matches(), "fallback build fingerprint must match defaults");

  axio::config::AxioConfig runtime_only = loaded;
  runtime_only.knobs.runtime.app_rx_batch_size += 1;
  expect(axio::compare_build_fingerprint(runtime_only).matches(),
         "runtime-only change must not alter the build fingerprint");

  axio::config::AxioConfig runtime_network = loaded;
  runtime_network.network.physical_port += 1;
  expect(axio::compare_build_fingerprint(runtime_network).matches(),
         "runtime network change must not alter the build fingerprint");

  axio::config::AxioConfig network_changed = loaded;
  network_changed.network.rx_ring_entries *= 2;
  expect(!axio::compare_build_fingerprint(network_changed).matches(),
         "projected network change must fail fingerprint match");

  axio::config::AxioConfig handler_changed = loaded;
  handler_changed.handler.message_handler =
      axio::config::MessageHandler::kLatency;
  expect(!axio::compare_build_fingerprint(handler_changed).matches(),
         "projected handler change must fail fingerprint match");

  axio::config::AxioConfig build_changed = loaded;
  build_changed.knobs.build.mtu *= 2;
  const axio::BuildFingerprintComparison mismatch =
      axio::compare_build_fingerprint(build_changed);
  expect(!mismatch.matches(), "build-time change must fail fingerprint match");
  expect(mismatch.embedded_fingerprint != mismatch.config_fingerprint,
         "mismatch report must retain both fingerprints");
}

void test_compiled_build_adapter(const axio::config::AxioConfig& loaded) {
  const axio::config::AxioConfig compiled = axio::compiled_build_config();
  expect(compiled.schema_version == loaded.schema_version,
         "schema version was not compiled");
  expect(compiled.deployment.role == loaded.deployment.role,
         "deployment role was not compiled");
  expect(compiled.network.backend == loaded.network.backend &&
             compiled.network.roce_transport == loaded.network.roce_transport,
         "backend selection was not compiled");
  expect(compiled.network.rx_ring_entries == loaded.network.rx_ring_entries &&
             compiled.network.tx_ring_entries ==
                 loaded.network.tx_ring_entries,
         "ring sizes were not compiled");
  expect(compiled.handler.message_handler == loaded.handler.message_handler &&
             compiled.handler.packet_handler == loaded.handler.packet_handler &&
             compiled.handler.apply_new_mbuf == loaded.handler.apply_new_mbuf &&
             compiled.handler.request_payload_bytes ==
                 loaded.handler.request_payload_bytes &&
             compiled.handler.response_payload_bytes ==
                 loaded.handler.response_payload_bytes &&
             compiled.handler.app_ticks_per_message ==
                 loaded.handler.app_ticks_per_message,
         "handler configuration was not compiled");
  expect(compiled.knobs.build.inflight_limit_enabled ==
                 loaded.knobs.build.inflight_limit_enabled &&
             compiled.knobs.build.inflight_messages ==
                 loaded.knobs.build.inflight_messages &&
             compiled.knobs.build.mtu == loaded.knobs.build.mtu &&
             compiled.knobs.build.mempool_handler ==
                 loaded.knobs.build.mempool_handler,
         "build knobs were not compiled");
  expect(compiled.other.mempool_size == loaded.other.mempool_size &&
             compiled.other.mempool_cache_size ==
                 loaded.other.mempool_cache_size,
         "mempool configuration was not compiled");
}

void test_runtime_adapter(const axio::config::AxioConfig& loaded) {
  const axio::UserConfig runtime(loaded);
  expect(runtime.numa_node() == 0, "NUMA node was not adapted");
  expect(runtime.physical_port() == 0, "physical port was not adapted");
  expect(runtime.iteration_count() == 30, "iteration count was not adapted");
  expect(runtime.duration_seconds() == 1, "window duration was not adapted");
  expect(std::strcmp(runtime.server().local_ip_, "10.0.0.1") == 0,
         "local IP was not adapted");
  expect(std::strcmp(runtime.server().device_pcie_address_, "0000:98:00.0") == 0,
         "PCIe BDF was not adapted canonically");
  expect(runtime.server().local_mac_[0] == 0x10 &&
             runtime.server().local_mac_[2] == 0xfd &&
             runtime.server().local_mac_[5] == 0x01,
         "local MAC was not adapted");
  expect(runtime.tunables().app_core_count_ == 1,
         "application core count was not adapted");
  expect(runtime.tunables().dispatcher_queue_count_ == 1,
         "dispatcher queue count was not adapted");
  expect(runtime.tunables().app_tx_message_batch_size_ == 32 &&
             runtime.tunables().app_rx_message_batch_size_ == 32 &&
             runtime.tunables().dispatcher_tx_batch_size_ == 32 &&
             runtime.tunables().dispatcher_rx_batch_size_ == 32 &&
             runtime.tunables().nic_tx_post_size_ == 32 &&
             runtime.tunables().nic_rx_post_size_ == 32,
         "runtime batch/post knobs were not adapted");

  const axio::config::ValidatedTopology& topology = runtime.topology();
  expect(topology.active_workspace_ids().size() == 2,
         "runtime must expose only active topology workspaces");
  expect(topology.workspace(axio::config::WorkspaceId(4)).cpu_core ==
             axio::config::CpuCoreId(4),
         "runtime must preserve the declared NUMA-local CPU core");
  expect(runtime.tunables().app_core_count_ ==
             topology.application_core_count(),
         "C1 must be derived from the validated topology");
  expect(runtime.tunables().dispatcher_queue_count_ ==
             topology.dispatcher_queue_count(),
         "C2 must be derived from the validated topology");

  const axio::UserConfig::WorkloadsConfig& workloads = runtime.workloads();
  expect(workloads.size() == 1, "workload count was not adapted");
  expect(workloads.pipeline_phases_.at(1).size() == 6,
         "pipeline phases were not adapted");
  expect(workloads.application_workspaces_.at(1).at(0).at(0) == 4,
         "application mapping was not adapted");
  expect(workloads.dispatchers_.at(1).at(0) == 0,
         "dispatcher mapping was not adapted");
  expect(workloads.remote_dispatchers_.at(1).at(0) == 0,
         "remote dispatcher mapping was not adapted");
}

void test_startup_summary(const axio::config::AxioConfig& loaded) {
  axio::config::AxioConfig sensitive = loaded;
  sensitive.deployment.host = "secret-host.example.net";
  sensitive.deployment.ssh_user = "secret-user";
  sensitive.deployment.workdir = "/secret/workdir";
  const axio::UserConfig runtime(sensitive);
  const std::string& summary = runtime.startup_summary();
  for (const char* expected : {
           "schema_version=1",
           "deployment.role=server",
           "deployment.numa_node=0",
           "network.backend=dpdk",
           "network.roce_transport=rc",
           "network.physical_port=0",
           "network.rx_ring_entries=2048",
           "network.tx_ring_entries=2048",
           "network.local_ip=10.0.0.1",
           "network.remote_ip=10.0.0.2",
           "network.local_mac=10:70:fd:00:00:01",
           "network.remote_mac=10:70:fd:00:00:02",
           "network.device_pcie=0000:98:00.0",
           "network.device_name=mlx5_0",
           "handler.message_handler=t_app",
           "handler.packet_handler=empty",
           "handler.apply_new_mbuf=false",
           "handler.request_payload_bytes=982",
           "handler.response_payload_bytes=22",
           "handler.app_ticks_per_message=0",
           "knobs.build.inflight_limit_enabled=true",
           "knobs.build.inflight_messages=1024",
           "knobs.build.mtu=2048",
           "knobs.build.mempool_handler=ring_mp_mc",
           "knobs.runtime.application_core_count=1",
           "knobs.runtime.dispatcher_queue_count=1",
           "knobs.runtime.app_tx_batch_size=32",
           "knobs.runtime.app_rx_batch_size=32",
           "knobs.runtime.dispatcher_tx_batch_size=32",
           "knobs.runtime.dispatcher_rx_batch_size=32",
           "knobs.runtime.nic_tx_post_size=32",
           "knobs.runtime.nic_rx_post_size=32",
           "other.iterations=30",
           "other.window_seconds=1",
           "other.mempool_size=8192",
           "other.mempool_cache_size=0",
           "reserved.metrics.jsonl_path=results/axio.jsonl",
           "reserved.metrics.human_output=true",
           "control_plane.tuning.max_iterations=20",
           "control_plane.tuning.latency_slo_us=100",
           "control_plane.tuning.warmup_windows=10",
           "control_plane.tuning.sample_windows=20",
           "control_plane.tuning.infrastructure_failure_limit=2",
           "control_plane.tuning.noise.throughput_relative_floor=0.01",
           "control_plane.tuning.noise.latency_relative_floor=0.03",
           "control_plane.tuning.noise.stage_time_relative_floor=0.05",
           "control_plane.tuning.noise.stall_time_relative_floor=0.05",
           "control_plane.tuning.noise.miss_rate_percentage_point_floor=0.5",
           "deployment.topology.application_workspaces=4",
           "deployment.topology.dispatcher_workspaces=0",
           "topology.workspace_count=2",
           "topology.workload_count=1",
       }) {
    const std::string expected_line = std::string(expected) + '\n';
    expect(summary.find(expected_line) != std::string::npos,
           std::string("startup summary omitted ") + expected);
  }
  expect(summary.find("secret-host") == std::string::npos &&
             summary.find("secret-user") == std::string::npos &&
             summary.find("/secret/workdir") == std::string::npos,
         "startup summary exposed deployment credentials");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      throw std::runtime_error("usage: runtime-config-test <source-root>");
    }
    const fs::path fixture =
        fs::path(argv[1]) / "tests/config/schema-v1.valid.toml";
    const axio::config::AxioConfig loaded = axio::config::load_config(fixture);
    const axio::config::ValidationResult validation =
        axio::config::validate_config(loaded);
    expect(validation.ok(), "fixture must validate: " + validation.format());
    test_fingerprint_boundary(loaded);
    test_compiled_build_adapter(loaded);
    test_runtime_adapter(loaded);
    test_startup_summary(loaded);
    std::cout << "Axio runtime config test passed" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio runtime config test failed: " << error.what() << std::endl;
    return 1;
  }
}
