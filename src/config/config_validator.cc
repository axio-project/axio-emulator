/**
 * @file config_validator.cc
 * @brief Validate Axio TOML schema-v1 values and cross-field constraints.
 */
#include "axio/config/config_validator.h"

#include <arpa/inet.h>

#include <algorithm>
#include <regex>
#include <sstream>
#include <utility>

namespace axio::config {
namespace {

bool is_power_of_two(uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
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

void validate_positive_batch(std::vector<ValidationIssue>* issues,
                             const AxioConfig& config, const std::string& key,
                             uint32_t value) {
  if (value == 0 || value > 512) {
    add_issue(issues, config, key, "must be between 1 and 512");
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

  if (config.build.mtu < 64 || config.build.mtu > 65535) {
    add_issue(&issues, config, "build.mtu", "must be between 64 and 65535");
  }
  if (!is_power_of_two(config.build.rx_ring_entries)) {
    add_issue(&issues, config, "build.rx_ring_entries",
              "must be a non-zero power of two");
  }
  if (!is_power_of_two(config.build.tx_ring_entries)) {
    add_issue(&issues, config, "build.tx_ring_entries",
              "must be a non-zero power of two");
  }
  const uint64_t minimum_mempool =
      static_cast<uint64_t>(config.build.rx_ring_entries) +
      config.build.tx_ring_entries;
  if (config.build.mempool_size < minimum_mempool) {
    add_issue(&issues, config, "build.mempool_size",
              "must cover the RX and TX rings");
  }
  if (config.build.mempool_cache_size > config.build.mempool_size) {
    add_issue(&issues, config, "build.mempool_cache_size",
              "must not exceed mempool_size");
  }
  if (config.build.request_payload_bytes > config.build.mtu) {
    add_issue(&issues, config, "build.request_payload_bytes",
              "must not exceed MTU");
  }
  if (config.build.response_payload_bytes > config.build.mtu) {
    add_issue(&issues, config, "build.response_payload_bytes",
              "must not exceed MTU");
  }
  if (config.build.inflight_limit_enabled &&
      config.build.inflight_messages == 0) {
    add_issue(&issues, config, "build.inflight_messages",
              "must be positive when the inflight limit is enabled");
  }
  if (config.build.inflight_messages > config.build.mempool_size) {
    add_issue(&issues, config, "build.inflight_messages",
              "must not exceed mempool_size");
  }

  if (config.runtime.iterations == 0) {
    add_issue(&issues, config, "runtime.iterations", "must be positive");
  }
  if (config.runtime.window_seconds == 0) {
    add_issue(&issues, config, "runtime.window_seconds", "must be positive");
  }
  validate_positive_batch(&issues, config, "runtime.app_tx_batch_size",
                          config.runtime.app_tx_batch_size);
  validate_positive_batch(&issues, config, "runtime.app_rx_batch_size",
                          config.runtime.app_rx_batch_size);
  validate_positive_batch(&issues, config, "runtime.dispatcher_tx_batch_size",
                          config.runtime.dispatcher_tx_batch_size);
  validate_positive_batch(&issues, config, "runtime.dispatcher_rx_batch_size",
                          config.runtime.dispatcher_rx_batch_size);
  validate_positive_batch(&issues, config, "runtime.nic_tx_post_size",
                          config.runtime.nic_tx_post_size);
  validate_positive_batch(&issues, config, "runtime.nic_rx_post_size",
                          config.runtime.nic_rx_post_size);
  if (config.runtime.nic_tx_post_size > config.build.tx_ring_entries) {
    add_issue(&issues, config, "runtime.nic_tx_post_size",
              "must not exceed tx_ring_entries");
  }
  if (config.runtime.nic_rx_post_size > config.build.rx_ring_entries) {
    add_issue(&issues, config, "runtime.nic_rx_post_size",
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
  if (config.deployment.host.empty()) {
    add_issue(&issues, config, "deployment.host", "must not be empty");
  }
  if (config.deployment.ssh_port == 0 || config.deployment.ssh_port > 65535) {
    add_issue(&issues, config, "deployment.ssh_port",
              "must be between 1 and 65535");
  }
  if (config.deployment.ssh_user.empty()) {
    add_issue(&issues, config, "deployment.ssh_user", "must not be empty");
  }
  if (config.deployment.workdir.empty()) {
    add_issue(&issues, config, "deployment.workdir", "must not be empty");
  }

  if (config.tuning.max_iterations == 0) {
    add_issue(&issues, config, "tuning.max_iterations", "must be positive");
  }
  if (config.tuning.latency_slo_us <= 0.0) {
    add_issue(&issues, config, "tuning.latency_slo_us", "must be positive");
  }
  if (config.tuning.sample_windows == 0) {
    add_issue(&issues, config, "tuning.sample_windows", "must be positive");
  }
  if (config.tuning.infrastructure_failure_limit == 0) {
    add_issue(&issues, config, "tuning.infrastructure_failure_limit",
              "must be positive");
  }
  validate_relative_floor(&issues, config,
                          "tuning.noise.throughput_relative_floor",
                          config.tuning.noise.throughput_relative_floor);
  validate_relative_floor(&issues, config,
                          "tuning.noise.latency_relative_floor",
                          config.tuning.noise.latency_relative_floor);
  validate_relative_floor(&issues, config,
                          "tuning.noise.stage_time_relative_floor",
                          config.tuning.noise.stage_time_relative_floor);
  validate_relative_floor(&issues, config,
                          "tuning.noise.stall_time_relative_floor",
                          config.tuning.noise.stall_time_relative_floor);
  const double miss_floor =
      config.tuning.noise.miss_rate_percentage_point_floor;
  if (miss_floor < 0.0 || miss_floor > 100.0) {
    add_issue(&issues, config,
              "tuning.noise.miss_rate_percentage_point_floor",
              "must be between 0.0 and 100.0");
  }

  if (config.workspaces.empty()) {
    add_issue(&issues, config, "workspaces", "must not be empty");
  }
  if (config.workloads.empty()) {
    add_issue(&issues, config, "workloads", "must not be empty");
  }

  return ValidationResult(std::move(issues));
}

}  // namespace axio::config
