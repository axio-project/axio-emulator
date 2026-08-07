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
  expect(runtime.tunables().nic_rx_post_size_ == 32,
         "NIC RX post size was not adapted");

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
    test_runtime_adapter(loaded);
    std::cout << "Axio runtime config test passed" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio runtime config test failed: " << error.what() << std::endl;
    return 1;
  }
}
