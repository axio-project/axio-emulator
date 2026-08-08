/**
 * @file config_validator.cc
 * @brief Validate Axio TOML schema-v1 values and cross-field constraints.
 */
#include "axio/config/config_validator.h"
#include "axio/config/backend_capabilities.h"
#include "axio/config/runtime_limits.h"
#include "axio/config/topology.h"

#include <arpa/inet.h>

#include <algorithm>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace axio::config {
namespace {

bool is_power_of_two(uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

bool is_message_handler(MessageHandler handler) {
  switch (handler) {
    case MessageHandler::kEmpty:
    case MessageHandler::kThroughput:
    case MessageHandler::kLatency:
    case MessageHandler::kMemory:
    case MessageHandler::kFileWrite:
    case MessageHandler::kFileRead:
    case MessageHandler::kKeyValue: return true;
  }
  return false;
}

uint64_t packet_count(uint32_t payload_bytes, uint32_t mtu) {
  constexpr uint32_t kIpv4HeaderBytes = 20;
  constexpr uint32_t kUdpHeaderBytes = 8;
  const uint64_t maximum_payload =
      static_cast<uint64_t>(mtu) - kIpv4HeaderBytes - kUdpHeaderBytes;
  return (payload_bytes + maximum_payload - 1) / maximum_payload;
}

SourceLocation source_for(const AxioConfig& config, const std::string& key) {
  const auto source = config.source_locations.find(key);
  if (source != config.source_locations.end()) {
    return source->second;
  }
  return {config.source_path.string(), 1, 1};
}

void add_issue(std::vector<ValidationIssue>* issues, const AxioConfig& config,
               const std::string& key, const std::string& message) {
  issues->push_back({key, source_for(config, key), message});
}

bool is_ipv4(const std::string& value) {
  in_addr address{};
  return inet_pton(AF_INET, value.c_str(), &address) == 1;
}

bool is_mac(const std::string& value) {
  static const std::regex pattern(
      "^[[:xdigit:]]{2}(:[[:xdigit:]]{2}){5}$",
      std::regex::ECMAScript | std::regex::icase);
  return std::regex_match(value, pattern);
}

bool is_pcie_bdf(const std::string& value) {
  static const std::regex pattern(
      "^[[:xdigit:]]{4}:[[:xdigit:]]{2}:[[:xdigit:]]{2}\\.[0-7]$",
      std::regex::ECMAScript | std::regex::icase);
  return std::regex_match(value, pattern);
}

bool is_ssh_placeholder(const std::string& value) {
  return value.empty() || value == "legacy-unset";
}

void validate_positive_batch(std::vector<ValidationIssue>* issues,
                             const AxioConfig& config, const std::string& key,
                             uint32_t value) {
  if (value == 0 || value > kMaximumApplicationBatchSize) {
    add_issue(issues, config, key,
              "must be between 1 and " +
                  std::to_string(kMaximumApplicationBatchSize));
  }
}

void validate_relative_floor(std::vector<ValidationIssue>* issues,
                             const AxioConfig& config, const std::string& key,
                             double value) {
  if (value < 0.0 || value > 1.0) {
    add_issue(issues, config, key, "must be between 0.0 and 1.0");
  }
}

}  // namespace

ValidationResult::ValidationResult(std::vector<ValidationIssue> issues)
    : issues_(std::move(issues)) {}

std::string ValidationResult::format() const {
  std::ostringstream output;
  for (size_t index = 0; index < this->issues_.size(); ++index) {
    if (index != 0) {
      output << '\n';
    }
    const ValidationIssue& issue = this->issues_[index];
    output << issue.source.format() << ": " << issue.key << ": "
           << issue.message;
  }
  return output.str();
}

ValidationResult validate_config(const AxioConfig& config) {
  std::vector<ValidationIssue> issues;
  const BackendCapabilities* capabilities = nullptr;
  try {
    capabilities = &capabilities_for(config.network.backend);
  } catch (const std::invalid_argument&) {
    add_issue(&issues, config, "network.backend",
              "is not a registered backend");
  }

  if (capabilities != nullptr &&
      !capabilities->supports_mtu(config.knobs.build.mtu)) {
    add_issue(&issues, config, "knobs.build.mtu",
              "is not supported by the selected backend");
  }
  if (capabilities != nullptr &&
      !capabilities->supports_mempool_handler(
          config.knobs.build.mempool_handler)) {
    add_issue(&issues, config, "knobs.build.mempool_handler",
              "is not supported by the selected backend");
  }
  if (capabilities != nullptr &&
      !capabilities->supports_packet_handler(config.handler.packet_handler)) {
    add_issue(&issues, config, "handler.packet_handler",
              "is not supported by the selected backend");
  }
  if (!is_message_handler(config.handler.message_handler)) {
    add_issue(&issues, config, "handler.message_handler",
              "is not a registered message handler");
  }
  if (capabilities != nullptr &&
      !capabilities->supports_rx_ring_entries(
          config.network.rx_ring_entries)) {
    add_issue(&issues, config, "network.rx_ring_entries",
              "is not supported by the selected backend");
  }
  if (!is_power_of_two(config.network.tx_ring_entries)) {
    add_issue(&issues, config, "network.tx_ring_entries",
              "must be a non-zero power of two");
  }
  uint64_t minimum_usable_mempool =
      static_cast<uint64_t>(config.network.rx_ring_entries) +
      config.network.tx_ring_entries;
  if (capabilities != nullptr &&
      capabilities->supports_mtu(config.knobs.build.mtu) &&
      config.handler.request_payload_bytes > 0 &&
      config.handler.response_payload_bytes > 0) {
    const uint64_t request_generation_batch =
        packet_count(config.handler.request_payload_bytes,
                     config.knobs.build.mtu) *
        config.knobs.runtime.app_tx_batch_size;
    const uint64_t request_packet_count =
        packet_count(config.handler.request_payload_bytes,
                     config.knobs.build.mtu);
    const uint64_t response_packet_count =
        packet_count(config.handler.response_payload_bytes,
                     config.knobs.build.mtu);
    const uint64_t receive_packet_count =
        config.deployment.role == Role::kClient ? response_packet_count
                                                : request_packet_count;
    uint64_t receive_processing_batch =
        receive_packet_count * config.knobs.runtime.app_rx_batch_size;
    if (config.deployment.role == Role::kServer &&
        config.handler.apply_new_mbuf) {
      receive_processing_batch +=
          response_packet_count * config.knobs.runtime.app_rx_batch_size;
    }
    std::map<uint32_t, uint64_t> applications_per_dispatcher;
    for (const WorkloadConfig& workload : config.deployment.topology.workloads) {
      for (const WorkloadGroupConfig& group : workload.groups) {
        applications_per_dispatcher[group.dispatcher] +=
            group.applications.size();
      }
    }
    uint64_t maximum_shared_applications = 0;
    for (const auto& [dispatcher, application_count] :
         applications_per_dispatcher) {
      static_cast<void>(dispatcher);
      maximum_shared_applications =
          std::max(maximum_shared_applications, application_count);
    }
    const uint64_t concurrent_application_demand =
        std::max(request_generation_batch, receive_processing_batch) *
        maximum_shared_applications;
    minimum_usable_mempool += concurrent_application_demand;

    if (receive_packet_count * config.knobs.runtime.app_rx_batch_size >
        kApplicationQueueEntries) {
      add_issue(&issues, config, "knobs.runtime.app_rx_batch_size",
                "packetized receive batch exceeds application scratch "
                "capacity");
    }
    if (config.deployment.role == Role::kServer &&
        config.handler.apply_new_mbuf &&
        response_packet_count * config.knobs.runtime.app_rx_batch_size >
            kApplicationQueueEntries) {
      add_issue(&issues, config, "knobs.runtime.app_rx_batch_size",
                "packetized response batch exceeds application scratch "
                "capacity");
    }
    if (config.deployment.role == Role::kServer &&
        !config.handler.apply_new_mbuf &&
        response_packet_count > request_packet_count) {
      add_issue(&issues, config, "handler.apply_new_mbuf",
                "must be enabled when a response uses more packets than its "
                "request");
    }
    const bool single_packet_message_handler =
        config.handler.message_handler == MessageHandler::kThroughput ||
        config.handler.message_handler == MessageHandler::kLatency ||
        config.handler.message_handler == MessageHandler::kMemory ||
        config.handler.message_handler == MessageHandler::kKeyValue;
    if (single_packet_message_handler &&
        (request_packet_count != 1 || response_packet_count != 1)) {
      add_issue(&issues, config, "handler.message_handler",
                "supports only single-packet requests and responses");
    }
    if (config.handler.message_handler == MessageHandler::kFileWrite &&
        response_packet_count != 1) {
      add_issue(&issues, config, "handler.message_handler",
                "file_write supports only single-packet responses");
    }
    if (config.handler.message_handler == MessageHandler::kFileRead &&
        request_packet_count != 1) {
      add_issue(&issues, config, "handler.message_handler",
                "file_read supports only single-packet requests");
    }
    if (config.handler.message_handler == MessageHandler::kFileRead &&
        !config.handler.apply_new_mbuf) {
      add_issue(&issues, config, "handler.apply_new_mbuf",
                "must be enabled for the file_read handler");
    }
  }
  if (capabilities != nullptr &&
      capabilities->usable_mempool_entries(config.other.mempool_size) <
          minimum_usable_mempool) {
    add_issue(&issues, config, "other.mempool_size",
              "must cover RX/TX rings and concurrent packetized application "
              "batches sharing one dispatcher");
  }
  if (config.other.mempool_cache_size > config.other.mempool_size) {
    add_issue(&issues, config, "other.mempool_cache_size",
              "must not exceed mempool_size");
  }
  if (capabilities != nullptr &&
      !capabilities->supports_mempool_cache(
          config.other.mempool_size, config.other.mempool_cache_size)) {
    add_issue(&issues, config, "other.mempool_cache_size",
              "exceeds the selected backend capability");
  }
  if (config.handler.request_payload_bytes == 0) {
    add_issue(&issues, config, "handler.request_payload_bytes",
              "must be positive");
  }
  if (config.handler.response_payload_bytes == 0) {
    add_issue(&issues, config, "handler.response_payload_bytes",
              "must be positive");
  }
  if (config.knobs.build.inflight_limit_enabled &&
      config.knobs.build.inflight_messages == 0) {
    add_issue(&issues, config, "knobs.build.inflight_messages",
              "must be positive when the inflight limit is enabled");
  }
  if (config.knobs.build.inflight_limit_enabled &&
      config.knobs.build.inflight_messages <
          std::max(config.knobs.runtime.app_tx_batch_size,
                   config.knobs.runtime.app_rx_batch_size)) {
    add_issue(&issues, config, "knobs.build.inflight_messages",
              "must cover the largest application batch when enabled");
  }
  if (config.knobs.build.inflight_messages > config.other.mempool_size) {
    add_issue(&issues, config, "knobs.build.inflight_messages",
              "must not exceed mempool_size");
  }

  if (config.other.iterations == 0) {
    add_issue(&issues, config, "other.iterations", "must be positive");
  }
  if (config.other.window_seconds == 0) {
    add_issue(&issues, config, "other.window_seconds", "must be positive");
  }
  if (config.knobs.runtime.application_core_count == 0 ||
      config.knobs.runtime.application_core_count > 255) {
    add_issue(&issues, config, "knobs.runtime.application_core_count",
              "must be between 1 and 255");
  }
  if (config.knobs.runtime.dispatcher_queue_count == 0 ||
      config.knobs.runtime.dispatcher_queue_count > 255) {
    add_issue(&issues, config, "knobs.runtime.dispatcher_queue_count",
              "must be between 1 and 255");
  }
  validate_positive_batch(&issues, config, "knobs.runtime.app_tx_batch_size",
                          config.knobs.runtime.app_tx_batch_size);
  validate_positive_batch(&issues, config, "knobs.runtime.app_rx_batch_size",
                          config.knobs.runtime.app_rx_batch_size);
  validate_positive_batch(&issues, config,
                          "knobs.runtime.dispatcher_tx_batch_size",
                          config.knobs.runtime.dispatcher_tx_batch_size);
  validate_positive_batch(&issues, config,
                          "knobs.runtime.dispatcher_rx_batch_size",
                          config.knobs.runtime.dispatcher_rx_batch_size);
  validate_positive_batch(&issues, config, "knobs.runtime.nic_tx_post_size",
                          config.knobs.runtime.nic_tx_post_size);
  validate_positive_batch(&issues, config, "knobs.runtime.nic_rx_post_size",
                          config.knobs.runtime.nic_rx_post_size);
  if (config.knobs.runtime.nic_tx_post_size >
      config.network.tx_ring_entries) {
    add_issue(&issues, config, "knobs.runtime.nic_tx_post_size",
              "must not exceed tx_ring_entries");
  }
  if (config.knobs.runtime.nic_rx_post_size >
      config.network.rx_ring_entries) {
    add_issue(&issues, config, "knobs.runtime.nic_rx_post_size",
              "must not exceed rx_ring_entries");
  }

  if (!is_ipv4(config.network.local_ip)) {
    add_issue(&issues, config, "network.local_ip",
              "must be a canonical IPv4 address");
  }
  if (!is_ipv4(config.network.remote_ip)) {
    add_issue(&issues, config, "network.remote_ip",
              "must be a canonical IPv4 address");
  }
  if (!is_mac(config.network.local_mac)) {
    add_issue(&issues, config, "network.local_mac",
              "must be a colon-delimited MAC address");
  }
  if (!is_mac(config.network.remote_mac)) {
    add_issue(&issues, config, "network.remote_mac",
              "must be a colon-delimited MAC address");
  }
  if (!is_pcie_bdf(config.network.device_pcie)) {
    add_issue(&issues, config, "network.device_pcie",
              "must be a canonical PCIe BDF");
  }
  if (config.network.device_name.empty()) {
    add_issue(&issues, config, "network.device_name", "must not be empty");
  }

  if (config.metrics.jsonl_path.empty()) {
    add_issue(&issues, config, "metrics.jsonl_path", "must not be empty");
  }
  if (config.deployment.transport == DeploymentTransport::kSsh) {
    if (is_ssh_placeholder(config.deployment.host)) {
      add_issue(&issues, config, "deployment.host",
                "must identify the SSH endpoint");
    }
    if (config.deployment.ssh_port == 0 ||
        config.deployment.ssh_port > 65535) {
      add_issue(&issues, config, "deployment.ssh_port",
                "must be between 1 and 65535 for SSH transport");
    }
    if (is_ssh_placeholder(config.deployment.ssh_user)) {
      add_issue(&issues, config, "deployment.ssh_user",
                "must identify the SSH user");
    }
  }
  if (config.deployment.workdir.empty()) {
    add_issue(&issues, config, "deployment.workdir", "must not be empty");
  }

  if (config.tuning.has_value()) {
    const TuningConfig& tuning = *config.tuning;
    if (tuning.max_iterations == 0) {
      add_issue(&issues, config, "tuning.max_iterations", "must be positive");
    }
    if (tuning.latency_slo_us <= 0.0) {
      add_issue(&issues, config, "tuning.latency_slo_us", "must be positive");
    }
    if (tuning.sample_windows == 0) {
      add_issue(&issues, config, "tuning.sample_windows", "must be positive");
    }
    if (tuning.infrastructure_failure_limit == 0) {
      add_issue(&issues, config, "tuning.infrastructure_failure_limit",
                "must be positive");
    }
    validate_relative_floor(&issues, config,
                            "tuning.noise.throughput_relative_floor",
                            tuning.noise.throughput_relative_floor);
    validate_relative_floor(&issues, config,
                            "tuning.noise.latency_relative_floor",
                            tuning.noise.latency_relative_floor);
    validate_relative_floor(&issues, config,
                            "tuning.noise.stage_time_relative_floor",
                            tuning.noise.stage_time_relative_floor);
    validate_relative_floor(&issues, config,
                            "tuning.noise.stall_time_relative_floor",
                            tuning.noise.stall_time_relative_floor);
    const double miss_floor = tuning.noise.miss_rate_percentage_point_floor;
    if (miss_floor < 0.0 || miss_floor > 100.0) {
      add_issue(&issues, config,
                "tuning.noise.miss_rate_percentage_point_floor",
                "must be between 0.0 and 100.0");
    }
  }

  if (config.deployment.topology.workspaces.empty()) {
    add_issue(&issues, config, "deployment.topology.workspaces",
              "must not be empty");
  }
  if (config.deployment.topology.workloads.empty()) {
    add_issue(&issues, config, "deployment.topology.workloads",
              "must not be empty");
  }

  try {
    static_cast<void>(ValidatedTopology::from_config(config));
  } catch (const TopologyError& error) {
    add_issue(&issues, config, error.key(), error.message());
  }

  return ValidationResult(std::move(issues));
}

}  // namespace axio::config
