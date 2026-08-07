#include "axio/config/config_loader.h"
#include "axio/config/config_validator.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace config = axio::config;

namespace {

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void expect_valid(const config::AxioConfig& value, const std::string& label) {
  const config::ValidationResult result = config::validate_config(value);
  expect(result.ok(), label + " must be accepted: " + result.format());
}

void expect_invalid(const config::AxioConfig& value, const std::string& key,
                    const std::string& label) {
  const config::ValidationResult result = config::validate_config(value);
  expect(!result.ok(), label + " must be rejected");
  expect(result.format().find(key) != std::string::npos,
         label + " must identify " + key + ": " + result.format());
}

config::AxioConfig roce_config(const config::AxioConfig& base) {
  config::AxioConfig value = base;
  value.network.backend = config::Backend::kRoce;
  value.network.roce_transport = config::RoceTransport::kRc;
  value.knobs.build.mempool_handler = config::MempoolHandler::kHugeAlloc;
  return value;
}

void test_mempool_handler_matrix(const config::AxioConfig& base) {
  const std::vector<config::MempoolHandler> dpdk_handlers = {
      config::MempoolHandler::kRingMpMc,
      config::MempoolHandler::kRingSpSc,
      config::MempoolHandler::kRingMpSc,
      config::MempoolHandler::kRingSpMc,
      config::MempoolHandler::kRingMtRts,
      config::MempoolHandler::kRingMtHts,
      config::MempoolHandler::kStack,
      config::MempoolHandler::kLfStack,
      config::MempoolHandler::kBucket,
  };
  for (const config::MempoolHandler handler : dpdk_handlers) {
    config::AxioConfig value = base;
    value.network.backend = config::Backend::kDpdk;
    value.knobs.build.mempool_handler = handler;
    expect_valid(value, "DPDK mempool handler");

    value.network.backend = config::Backend::kRoce;
    expect_invalid(value, "knobs.build.mempool_handler",
                   "DPDK mempool handler on RoCE");
  }

  config::AxioConfig roce = roce_config(base);
  expect_valid(roce, "RoCE huge-page allocator");

  config::AxioConfig invalid_dpdk = base;
  invalid_dpdk.knobs.build.mempool_handler =
      config::MempoolHandler::kHugeAlloc;
  expect_invalid(invalid_dpdk, "knobs.build.mempool_handler",
                 "RoCE allocator on DPDK");

  config::AxioConfig unknown = base;
  unknown.knobs.build.mempool_handler =
      static_cast<config::MempoolHandler>(255);
  expect_invalid(unknown, "knobs.build.mempool_handler",
                 "unknown mempool handler");
}

void test_mtu_boundaries(const config::AxioConfig& base) {
  config::AxioConfig dpdk = base;
  dpdk.handler.request_payload_bytes = 22;
  dpdk.handler.response_payload_bytes = 22;
  dpdk.knobs.build.mtu = 64;
  expect_valid(dpdk, "minimum DPDK MTU");
  dpdk.knobs.build.mtu = 65535;
  expect_valid(dpdk, "maximum DPDK MTU");
  dpdk.knobs.build.mtu = 63;
  expect_invalid(dpdk, "knobs.build.mtu", "DPDK MTU below minimum");

  for (const uint32_t mtu : {1024U, 2048U, 4096U}) {
    config::AxioConfig roce = roce_config(base);
    roce.knobs.build.mtu = mtu;
    expect_valid(roce, "supported RoCE MTU");
  }
  for (const uint32_t mtu : {512U, 1500U, 8192U}) {
    config::AxioConfig roce = roce_config(base);
    roce.knobs.build.mtu = mtu;
    expect_invalid(roce, "knobs.build.mtu", "unsupported RoCE MTU");
  }
}

void test_ring_mempool_and_inflight_boundaries(
    const config::AxioConfig& base) {
  config::AxioConfig value = base;
  value.network.rx_ring_entries = 0;
  expect_invalid(value, "network.rx_ring_entries", "empty RX ring");

  value = base;
  value.network.tx_ring_entries = 3;
  expect_invalid(value, "network.tx_ring_entries", "non-power-of-two TX ring");

  value = base;
  value.knobs.runtime.nic_rx_post_size =
      value.network.rx_ring_entries + 1;
  expect_invalid(value, "knobs.runtime.nic_rx_post_size",
                 "RX post larger than ring");

  value = base;
  value.other.mempool_size =
      value.network.rx_ring_entries + value.network.tx_ring_entries - 1;
  expect_invalid(value, "other.mempool_size", "undersized mempool");

  value = base;
  value.other.mempool_cache_size = value.other.mempool_size + 1;
  expect_invalid(value, "other.mempool_cache_size", "oversized mempool cache");

  value = base;
  value.knobs.build.inflight_messages = 0;
  expect_invalid(value, "knobs.build.inflight_messages",
                 "empty enabled inflight budget");

  value = base;
  value.knobs.build.inflight_messages = value.other.mempool_size + 1;
  expect_invalid(value, "knobs.build.inflight_messages",
                 "inflight budget larger than mempool");

  value = base;
  value.knobs.build.inflight_limit_enabled = false;
  value.knobs.build.inflight_messages = 0;
  expect_valid(value, "disabled inflight budget");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    expect(argc == 2,
           "usage: axio-backend-capabilities-test <source-root>");
    const config::AxioConfig base = config::load_config(
        fs::path(argv[1]) / "tests/config/schema-v1.valid.toml");
    test_mempool_handler_matrix(base);
    test_mtu_boundaries(base);
    test_ring_mempool_and_inflight_boundaries(base);
    std::cout << "Axio backend capability matrix test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio backend capability matrix test failed: " << error.what()
              << '\n';
    return 1;
  }
}
