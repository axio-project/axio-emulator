#include "axio/config/backend_capabilities.h"
#include "axio/config/config_loader.h"
#include "axio/config/config_validator.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

void test_capability_descriptors() {
  const config::BackendCapabilities& dpdk =
      config::capabilities_for(config::Backend::kDpdk);
  expect(dpdk.mempool_handlers.size() == 9,
         "DPDK must publish all nine mempool handlers");
  expect(dpdk.packet_handlers ==
             std::vector<config::PacketHandler>(
                 {config::PacketHandler::kEmpty,
                  config::PacketHandler::kEcho}),
         "DPDK packet-handler capabilities changed");
  const std::vector<std::pair<config::MempoolHandler, std::string_view>>
      dpdk_ops = {
          {config::MempoolHandler::kRingMpMc, "ring_mp_mc"},
          {config::MempoolHandler::kRingSpSc, "ring_sp_sc"},
          {config::MempoolHandler::kRingMpSc, "ring_mp_sc"},
          {config::MempoolHandler::kRingSpMc, "ring_sp_mc"},
          {config::MempoolHandler::kRingMtRts, "ring_mt_rts"},
          {config::MempoolHandler::kRingMtHts, "ring_mt_hts"},
          {config::MempoolHandler::kStack, "stack"},
          {config::MempoolHandler::kLfStack, "lf_stack"},
          {config::MempoolHandler::kBucket, "bucket"},
      };
  for (const auto& [handler, name] : dpdk_ops) {
    expect(config::dpdk_mempool_ops_name(handler) == name,
           "DPDK handler name must match its registered ops name");
  }
  expect(dpdk.usable_mempool_entries(8192) == 8191,
         "DPDK pool creation reserves one configured entry");

  const config::BackendCapabilities& roce =
      config::capabilities_for(config::Backend::kRoce);
  expect(roce.mempool_handlers ==
             std::vector<config::MempoolHandler>(
                 {config::MempoolHandler::kHugeAlloc}) &&
             roce.packet_handlers ==
                 std::vector<config::PacketHandler>(
                     {config::PacketHandler::kEmpty}),
         "RoCE must publish only implemented handlers");
  expect(roce.usable_mempool_entries(8192) == 8192,
         "RoCE must expose every configured pool entry");
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
  dpdk.knobs.build.mtu = 32768;
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

void test_ring_boundaries(const config::AxioConfig& base) {
  config::AxioConfig roce = roce_config(base);
  roce.network.rx_ring_entries = 2048;
  expect_valid(roce, "maximum RoCE RX ring");

  roce.network.rx_ring_entries = 4096;
  expect_invalid(roce, "network.rx_ring_entries",
                 "RoCE RX ring above allocator limit");
}

void test_packet_and_message_handler_boundaries(
    const config::AxioConfig& base) {
  config::AxioConfig dpdk_echo = base;
  dpdk_echo.handler.packet_handler = config::PacketHandler::kEcho;
  expect_valid(dpdk_echo, "DPDK echo packet handler");

  config::AxioConfig roce_echo = roce_config(base);
  roce_echo.handler.packet_handler = config::PacketHandler::kEcho;
  expect_invalid(roce_echo, "handler.packet_handler",
                 "RoCE echo packet handler");

  config::AxioConfig unknown_packet = base;
  unknown_packet.handler.packet_handler =
      static_cast<config::PacketHandler>(255);
  expect_invalid(unknown_packet, "handler.packet_handler",
                 "unknown packet handler");

  config::AxioConfig unknown_message = base;
  unknown_message.handler.message_handler =
      static_cast<config::MessageHandler>(255);
  expect_invalid(unknown_message, "handler.message_handler",
                 "unknown message handler");
}

void test_payload_and_derived_pool_capacity(const config::AxioConfig& base) {
  config::AxioConfig empty_request = base;
  empty_request.handler.request_payload_bytes = 0;
  expect_invalid(empty_request, "handler.request_payload_bytes",
                 "empty request payload");

  config::AxioConfig empty_response = base;
  empty_response.handler.response_payload_bytes = 0;
  expect_invalid(empty_response, "handler.response_payload_bytes",
                 "empty response payload");

  config::AxioConfig multi_packet = base;
  multi_packet.handler.message_handler =
      config::MessageHandler::kFileWrite;
  multi_packet.knobs.build.mtu = 1024;
  multi_packet.handler.request_payload_bytes = 16384;
  multi_packet.handler.response_payload_bytes = 22;
  multi_packet.knobs.runtime.app_tx_batch_size = 512;
  multi_packet.knobs.runtime.app_rx_batch_size = 128;
  multi_packet.other.mempool_size = 16384;
  expect_valid(multi_packet, "multi-packet message capacity");

  multi_packet.other.mempool_size = 8192;
  expect_invalid(multi_packet, "other.mempool_size",
                 "mempool below derived multi-packet batch demand");

  config::AxioConfig exact_dpdk = base;
  const uint32_t exact_demand = exact_dpdk.network.rx_ring_entries +
                                exact_dpdk.network.tx_ring_entries +
                                exact_dpdk.knobs.runtime.app_tx_batch_size;
  exact_dpdk.other.mempool_size = exact_demand;
  expect_invalid(exact_dpdk, "other.mempool_size",
                 "DPDK configured pool without creation reserve");
  exact_dpdk.other.mempool_size = exact_demand + 1;
  expect_valid(exact_dpdk, "DPDK configured pool with creation reserve");

  config::AxioConfig shared_dispatcher = exact_dpdk;
  shared_dispatcher.workspaces.push_back({5, 5});
  shared_dispatcher.tuning.resources.application_workspaces.push_back(5);
  shared_dispatcher.workloads.front().groups.front().applications.push_back(5);
  shared_dispatcher.knobs.runtime.application_core_count = 2;
  expect_invalid(shared_dispatcher, "other.mempool_size",
                 "shared dispatcher application demand");

  config::AxioConfig asymmetric_server = multi_packet;
  asymmetric_server.knobs.runtime.app_tx_batch_size = 1;
  asymmetric_server.other.mempool_size = 6272;
  expect_invalid(asymmetric_server, "other.mempool_size",
                 "server receive batch demand");
  asymmetric_server.other.mempool_size = 6273;
  expect_valid(asymmetric_server, "server receive batch capacity boundary");

  config::AxioConfig new_buffer_server = base;
  new_buffer_server.handler.message_handler =
      config::MessageHandler::kFileRead;
  new_buffer_server.handler.apply_new_mbuf = true;
  new_buffer_server.handler.request_payload_bytes = 22;
  new_buffer_server.handler.response_payload_bytes = 3000;
  new_buffer_server.other.mempool_size = 4192;
  expect_invalid(new_buffer_server, "other.mempool_size",
                 "coexisting server request and response buffers");
  new_buffer_server.other.mempool_size = 4193;
  expect_valid(new_buffer_server,
               "new-buffer server capacity boundary");
}

void test_message_buffer_boundaries(const config::AxioConfig& base) {
  config::AxioConfig unsafe_reuse = base;
  unsafe_reuse.handler.request_payload_bytes = 22;
  unsafe_reuse.handler.response_payload_bytes = 3000;
  expect_invalid(unsafe_reuse, "handler.apply_new_mbuf",
                 "response larger than reusable request buffers");

  config::AxioConfig unsafe_per_packet_handler = unsafe_reuse;
  unsafe_per_packet_handler.handler.apply_new_mbuf = true;
  expect_invalid(unsafe_per_packet_handler, "handler.message_handler",
                 "per-packet handler with unequal segment counts");

  unsafe_per_packet_handler.handler.request_payload_bytes = 3000;
  expect_invalid(unsafe_per_packet_handler, "handler.message_handler",
                 "per-packet handler with equal multi-packet messages");

  config::AxioConfig unsafe_file_write = unsafe_per_packet_handler;
  unsafe_file_write.handler.message_handler =
      config::MessageHandler::kFileWrite;
  expect_invalid(unsafe_file_write, "handler.message_handler",
                 "file-write multi-packet response");

  config::AxioConfig unsafe_file_read = base;
  unsafe_file_read.handler.message_handler =
      config::MessageHandler::kFileRead;
  expect_invalid(unsafe_file_read, "handler.apply_new_mbuf",
                 "file-read buffer reuse");

  config::AxioConfig oversized_receive_batch = base;
  oversized_receive_batch.handler.message_handler =
      config::MessageHandler::kFileWrite;
  oversized_receive_batch.knobs.build.mtu = 1024;
  oversized_receive_batch.handler.request_payload_bytes = 16384;
  oversized_receive_batch.knobs.runtime.app_rx_batch_size = 512;
  oversized_receive_batch.other.mempool_size = 32768;
  expect_invalid(oversized_receive_batch,
                 "knobs.runtime.app_rx_batch_size",
                 "receive batch larger than application scratch space");
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
  value.other.mempool_cache_size = 513;
  expect_invalid(value, "other.mempool_cache_size",
                 "DPDK cache above backend maximum");

  value = base;
  value.network.rx_ring_entries = 64;
  value.network.tx_ring_entries = 64;
  value.other.mempool_size = 768;
  value.other.mempool_cache_size = 512;
  value.knobs.build.inflight_messages = 64;
  expect_invalid(value, "other.mempool_cache_size",
                 "DPDK cache violating create-by-ops ratio");
  value.other.mempool_size = 769;
  expect_valid(value, "DPDK cache at create-by-ops ratio boundary");

  value = roce_config(base);
  value.other.mempool_cache_size = 1;
  expect_invalid(value, "other.mempool_cache_size", "RoCE mempool cache");

  value = base;
  value.knobs.build.inflight_messages = 0;
  expect_invalid(value, "knobs.build.inflight_messages",
                 "empty enabled inflight budget");

  value = base;
  value.knobs.build.inflight_messages = value.other.mempool_size + 1;
  expect_invalid(value, "knobs.build.inflight_messages",
                 "inflight budget larger than mempool");

  value = base;
  value.knobs.build.inflight_messages = 16;
  expect_invalid(value, "knobs.build.inflight_messages",
                 "inflight budget below application batch");

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
    test_capability_descriptors();
    test_mempool_handler_matrix(base);
    test_mtu_boundaries(base);
    test_ring_boundaries(base);
    test_packet_and_message_handler_boundaries(base);
    test_payload_and_derived_pool_capacity(base);
    test_message_buffer_boundaries(base);
    test_ring_mempool_and_inflight_boundaries(base);
    std::cout << "Axio backend capability matrix test passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio backend capability matrix test failed: " << error.what()
              << '\n';
    return 1;
  }
}
