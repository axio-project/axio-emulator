/**
 * @file legacy_converter.cc
 * @brief Convert Axio's colon-delimited configuration into schema-v1 values.
 */
#include "axio/config/config_loader.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace axio::config {
namespace {

struct LegacyValue {
  std::string value;
  uint32_t line = 1;
};

struct LegacyWorkload {
  std::vector<std::string> fields;
  uint32_t line = 1;
};

std::string trim(const std::string& value) {
  const size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::vector<std::string> split(const std::string& value, char delimiter) {
  std::vector<std::string> fields;
  std::istringstream input(value);
  std::string field;
  while (std::getline(input, field, delimiter)) {
    fields.push_back(trim(field));
  }
  return fields;
}

SourceLocation source(const std::filesystem::path& path, uint32_t line) {
  return {path.string(), line, 1};
}

const LegacyValue& required(const std::map<std::string, LegacyValue>& values,
                            const std::string& key,
                            const std::filesystem::path& path) {
  const auto value = values.find(key);
  if (value == values.end()) {
    throw ConfigError(key, source(path, 1), "required legacy key is missing");
  }
  return value->second;
}

uint32_t parse_u32(const std::string& key, const LegacyValue& value,
                   const std::filesystem::path& path) {
  try {
    size_t consumed = 0;
    const unsigned long parsed = std::stoul(value.value, &consumed, 10);
    if (consumed != value.value.size() || parsed > UINT32_MAX) {
      throw std::out_of_range("uint32");
    }
    return static_cast<uint32_t>(parsed);
  } catch (const std::exception&) {
    throw ConfigError(key, source(path, value.line),
                      "expected non-negative uint32 integer");
  }
}

uint32_t parse_u32(const std::string& key, const std::string& value,
                   const std::filesystem::path& path, uint32_t line) {
  return parse_u32(key, {value, line}, path);
}

std::vector<uint32_t> parse_ids(const std::string& key,
                                const std::string& value,
                                const std::filesystem::path& path,
                                uint32_t line) {
  std::vector<uint32_t> ids;
  for (const std::string& token : split(value, ',')) {
    if (token.empty()) {
      throw ConfigError(key, source(path, line), "empty workspace ID");
    }
    ids.push_back(parse_u32(key, token, path, line));
  }
  return ids;
}

std::vector<uint32_t> parse_application_group(
    const std::string& value, const std::filesystem::path& path, uint32_t line) {
  const size_t dash = value.find('-');
  if (dash == std::string::npos) {
    return parse_ids("workload.applications", value, path, line);
  }
  const uint32_t first = parse_u32("workload.applications",
                                   value.substr(0, dash), path, line);
  const uint32_t last = parse_u32("workload.applications",
                                  value.substr(dash + 1), path, line);
  if (last < first) {
    throw ConfigError("workload.applications", source(path, line),
                      "workspace range must be ascending");
  }
  std::vector<uint32_t> ids;
  for (uint32_t id = first; id <= last; ++id) {
    ids.push_back(id);
  }
  return ids;
}

PipelinePhase parse_phase(const std::string& value,
                          const std::filesystem::path& path, uint32_t line) {
  if (value == "RXNIC") return PipelinePhase::kNicRx;
  if (value == "RXDispatcher") return PipelinePhase::kDispatcherRx;
  if (value == "RxApplication") return PipelinePhase::kApplicationRx;
  if (value == "TxApplication") return PipelinePhase::kApplicationTx;
  if (value == "TxDispatcher") return PipelinePhase::kDispatcherTx;
  if (value == "TxNIC") return PipelinePhase::kNicTx;
  throw ConfigError("workload.pipeline", source(path, line),
                    "unsupported legacy pipeline phase '" + value + "'");
}

std::string canonical_mac(std::string value) {
  std::replace(value.begin(), value.end(), '.', ':');
  return value;
}

std::string canonical_pcie(const std::string& value) {
  if (value.find(':') != std::string::npos) return value;
  const std::vector<std::string> fields = split(value, '.');
  if (fields.size() != 4) return value;
  return fields[0] + ":" + fields[1] + ":" + fields[2] + "." + fields[3];
}

void record_source(AxioConfig* config, const std::string& key,
                   const std::filesystem::path& path, uint32_t line) {
  config->source_locations[key] = source(path, line);
}

}  // namespace

AxioConfig load_legacy_config(const std::filesystem::path& path, Role role,
                              Backend backend) {
  std::ifstream input(path);
  if (!input.is_open()) {
    throw ConfigError("<legacy>", source(path, 1), "failed to open file");
  }

  std::map<std::string, LegacyValue> values;
  std::vector<LegacyWorkload> workloads;
  std::string line;
  uint32_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const std::string content = trim(line);
    if (content.empty() || content[0] == '#') continue;
    const std::vector<std::string> fields = split(content, ':');
    if (fields.size() < 2) {
      throw ConfigError("<legacy>", source(path, line_number),
                        "expected key:value entry");
    }
    if (fields[0] == "workload") {
      if (fields.size() != 6) {
        throw ConfigError("workload", source(path, line_number),
                          "expected six workload fields");
      }
      workloads.push_back({fields, line_number});
      continue;
    }
    if (fields.size() != 2) {
      throw ConfigError(fields[0], source(path, line_number),
                        "unexpected ':' in legacy value");
    }
    values[fields[0]] = {fields[1], line_number};
  }

  AxioConfig config;
  config.schema_version = 1;
  config.source_path = path;
  config.build.role = role;
  config.build.backend = backend;
  config.build.roce_transport = RoceTransport::kRc;
  config.build.mtu = 2048;
  config.build.rx_ring_entries = 2048;
  config.build.tx_ring_entries = 2048;
  config.build.mempool_size = 8192;
  config.build.mempool_handler = backend == Backend::kDpdk
                                     ? MempoolHandler::kRingMpMc
                                     : MempoolHandler::kHugeAlloc;
  config.build.mempool_cache_size = 0;
  config.build.message_handler = MessageHandler::kThroughput;
  config.build.packet_handler = PacketHandler::kEmpty;
  config.build.apply_new_mbuf = false;
  config.build.request_payload_bytes = 982;
  config.build.response_payload_bytes = 22;
  config.build.app_ticks_per_message = 0;
  config.build.inflight_limit_enabled = true;
  config.build.inflight_messages = 1024;

  const auto assign_runtime = [&](const std::string& legacy_key,
                                  const std::string& schema_key,
                                  uint32_t* destination) {
    const LegacyValue& value = required(values, legacy_key, path);
    *destination = parse_u32(schema_key, value, path);
    record_source(&config, schema_key, path, value.line);
  };
  assign_runtime("numa", "runtime.numa_node", &config.runtime.numa_node);
  assign_runtime("phy_port", "runtime.physical_port",
                 &config.runtime.physical_port);
  assign_runtime("iteration", "runtime.iterations", &config.runtime.iterations);
  assign_runtime("duration", "runtime.window_seconds",
                 &config.runtime.window_seconds);
  assign_runtime("kAppTxMsgBatchSize", "runtime.app_tx_batch_size",
                 &config.runtime.app_tx_batch_size);
  assign_runtime("kAppRxMsgBatchSize", "runtime.app_rx_batch_size",
                 &config.runtime.app_rx_batch_size);
  assign_runtime("kDispTxBatchSize", "runtime.dispatcher_tx_batch_size",
                 &config.runtime.dispatcher_tx_batch_size);
  assign_runtime("kDispRxBatchSize", "runtime.dispatcher_rx_batch_size",
                 &config.runtime.dispatcher_rx_batch_size);
  assign_runtime("kNICTxPostSize", "runtime.nic_tx_post_size",
                 &config.runtime.nic_tx_post_size);
  assign_runtime("kNICRxPostSize", "runtime.nic_rx_post_size",
                 &config.runtime.nic_rx_post_size);

  const auto assign_string = [&](const std::string& legacy_key,
                                 const std::string& schema_key,
                                 std::string* destination) {
    const LegacyValue& value = required(values, legacy_key, path);
    *destination = value.value;
    record_source(&config, schema_key, path, value.line);
  };
  assign_string("local_ip", "network.local_ip", &config.network.local_ip);
  assign_string("remote_ip", "network.remote_ip", &config.network.remote_ip);
  assign_string("local_mac", "network.local_mac", &config.network.local_mac);
  assign_string("remote_mac", "network.remote_mac", &config.network.remote_mac);
  assign_string("device_pcie", "network.device_pcie",
                &config.network.device_pcie);
  assign_string("device_name", "network.device_name",
                &config.network.device_name);
  config.network.local_mac = canonical_mac(config.network.local_mac);
  config.network.remote_mac = canonical_mac(config.network.remote_mac);
  config.network.device_pcie = canonical_pcie(config.network.device_pcie);

  config.metrics.jsonl_path = "results/axio.jsonl";
  config.metrics.human_output = true;
  config.deployment.host = "legacy-unset";
  config.deployment.ssh_port = 22;
  config.deployment.ssh_user = "legacy-unset";
  config.deployment.workdir = ".";
  config.deployment.use_sudo = true;
  config.tuning.max_iterations = 20;
  config.tuning.latency_slo_us = 100.0;
  config.tuning.warmup_windows = 10;
  config.tuning.sample_windows = 20;
  config.tuning.infrastructure_failure_limit = 2;
  config.tuning.noise = {0.01, 0.03, 0.05, 0.05, 0.5};

  std::set<uint32_t> local_workspace_ids;
  for (size_t workload_index = 0; workload_index < workloads.size();
       ++workload_index) {
    const LegacyWorkload& legacy = workloads[workload_index];
    const std::string prefix = "workloads[" + std::to_string(workload_index) + "]";
    WorkloadConfig workload;
    workload.id = parse_u32(prefix + ".id", legacy.fields[1], path, legacy.line);
    for (const std::string& phase : split(legacy.fields[2], ',')) {
      workload.pipeline.push_back(parse_phase(phase, path, legacy.line));
    }
    workload.remote_dispatchers =
        parse_ids(prefix + ".remote_dispatchers", legacy.fields[3], path,
                  legacy.line);

    const std::vector<std::string> application_groups =
        split(legacy.fields[4], '|');
    const std::vector<std::string> dispatcher_groups =
        split(legacy.fields[5], '|');
    if (application_groups.size() != dispatcher_groups.size()) {
      throw ConfigError(prefix + ".groups", source(path, legacy.line),
                        "application and dispatcher group counts differ");
    }
    for (size_t group_index = 0; group_index < application_groups.size();
         ++group_index) {
      WorkloadGroupConfig group;
      group.applications = parse_application_group(
          application_groups[group_index], path, legacy.line);
      const std::vector<uint32_t> dispatchers = parse_ids(
          prefix + ".groups.dispatcher", dispatcher_groups[group_index], path,
          legacy.line);
      if (dispatchers.size() != 1) {
        throw ConfigError(prefix + ".groups.dispatcher",
                          source(path, legacy.line),
                          "each legacy group must contain one dispatcher");
      }
      group.dispatcher = dispatchers.front();
      local_workspace_ids.insert(group.dispatcher);
      local_workspace_ids.insert(group.applications.begin(),
                                 group.applications.end());
      workload.groups.push_back(std::move(group));
    }
    record_source(&config, prefix + ".id", path, legacy.line);
    record_source(&config, prefix + ".pipeline", path, legacy.line);
    record_source(&config, prefix + ".remote_dispatchers", path, legacy.line);
    record_source(&config, prefix + ".groups", path, legacy.line);
    config.workloads.push_back(std::move(workload));
  }

  for (const uint32_t id : local_workspace_ids) {
    config.workspaces.push_back({id, id});
  }
  config.tuning.resources.application_workspaces.assign(
      local_workspace_ids.begin(), local_workspace_ids.end());
  config.tuning.resources.dispatcher_workspaces.assign(
      local_workspace_ids.begin(), local_workspace_ids.end());

  const SourceLocation fallback = source(path, 1);
  const std::vector<std::string> default_keys = {
      "schema_version",
      "build.mtu",
      "build.rx_ring_entries",
      "build.tx_ring_entries",
      "build.mempool_size",
      "build.mempool_cache_size",
      "build.request_payload_bytes",
      "build.response_payload_bytes",
      "build.inflight_messages",
      "metrics.jsonl_path",
      "deployment.host",
      "deployment.ssh_port",
      "deployment.ssh_user",
      "deployment.workdir",
      "tuning.max_iterations",
      "tuning.latency_slo_us",
      "tuning.sample_windows",
      "tuning.infrastructure_failure_limit",
      "tuning.noise.throughput_relative_floor",
      "tuning.noise.latency_relative_floor",
      "tuning.noise.stage_time_relative_floor",
      "tuning.noise.stall_time_relative_floor",
      "tuning.noise.miss_rate_percentage_point_floor",
      "workspaces",
      "workloads",
  };
  for (const std::string& key : default_keys) {
    config.source_locations.emplace(key, fallback);
  }

  return config;
}

}  // namespace axio::config
