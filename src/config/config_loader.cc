/**
 * @file config_loader.cc
 * @brief Strict TOML schema-v1 loader shared by Axio and its tooling.
 */
#include "axio/config/config_loader.h"

#include <toml++/toml.hpp>

#include <initializer_list>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

namespace axio::config {
namespace {

SourceLocation source_location(const toml::source_region& region,
                               const std::filesystem::path& fallback) {
  SourceLocation source;
  source.path = region.path ? *region.path : fallback.string();
  source.line = region.begin.line;
  source.column = region.begin.column;
  return source;
}

SourceLocation source_location(const toml::node& node,
                               const std::filesystem::path& fallback) {
  return source_location(node.source(), fallback);
}

std::string diagnostic(const std::string& key, const SourceLocation& source,
                       const std::string& message) {
  return source.format() + ": " + key + ": " + message;
}

bool contains(std::initializer_list<std::string_view> allowed,
              std::string_view key) {
  for (const std::string_view candidate : allowed) {
    if (candidate == key) {
      return true;
    }
  }
  return false;
}

void reject_unknown(const toml::table& table, const std::string& prefix,
                    std::initializer_list<std::string_view> allowed,
                    const std::filesystem::path& fallback) {
  for (const auto& [key, node] : table) {
    const std::string key_text(key.str());
    if (!contains(allowed, key_text)) {
      const std::string full_key =
          prefix.empty() ? key_text : prefix + "." + key_text;
      throw ConfigError(full_key, source_location(node, fallback),
                        "unknown key");
    }
  }
}

const toml::node& required_node(const toml::table& table, const std::string& key,
                                const std::string& full_key,
                                AxioConfig* config) {
  const toml::node* node = table.get(key);
  if (node == nullptr) {
    throw ConfigError(full_key, source_location(table, config->source_path),
                      "required key is missing");
  }
  config->source_locations[full_key] =
      source_location(*node, config->source_path);
  return *node;
}

const toml::table& required_table(const toml::table& parent,
                                  const std::string& key,
                                  const std::string& full_key,
                                  AxioConfig* config) {
  const toml::node& node = required_node(parent, key, full_key, config);
  const toml::table* table = node.as_table();
  if (table == nullptr) {
    throw ConfigError(full_key, source_location(node, config->source_path),
                      "expected table");
  }
  return *table;
}

const toml::array& required_array(const toml::table& parent,
                                  const std::string& key,
                                  const std::string& full_key,
                                  AxioConfig* config) {
  const toml::node& node = required_node(parent, key, full_key, config);
  const toml::array* array = node.as_array();
  if (array == nullptr) {
    throw ConfigError(full_key, source_location(node, config->source_path),
                      "expected array");
  }
  return *array;
}

std::string read_string(const toml::table& table, const std::string& key,
                        const std::string& full_key, AxioConfig* config) {
  const toml::node& node = required_node(table, key, full_key, config);
  const auto value = node.value<std::string>();
  if (!value) {
    throw ConfigError(full_key, source_location(node, config->source_path),
                      "expected string");
  }
  return *value;
}

bool read_bool(const toml::table& table, const std::string& key,
               const std::string& full_key, AxioConfig* config) {
  const toml::node& node = required_node(table, key, full_key, config);
  const auto value = node.value<bool>();
  if (!value) {
    throw ConfigError(full_key, source_location(node, config->source_path),
                      "expected boolean");
  }
  return *value;
}

uint64_t read_u64(const toml::table& table, const std::string& key,
                  const std::string& full_key, AxioConfig* config) {
  const toml::node& node = required_node(table, key, full_key, config);
  const auto value = node.value<int64_t>();
  if (!value || *value < 0) {
    throw ConfigError(full_key, source_location(node, config->source_path),
                      "expected non-negative integer");
  }
  return static_cast<uint64_t>(*value);
}

uint32_t read_u32(const toml::table& table, const std::string& key,
                  const std::string& full_key, AxioConfig* config) {
  const uint64_t value = read_u64(table, key, full_key, config);
  if (value > std::numeric_limits<uint32_t>::max()) {
    throw ConfigError(full_key, config->source_locations.at(full_key),
                      "integer exceeds uint32 range");
  }
  return static_cast<uint32_t>(value);
}

double read_double(const toml::table& table, const std::string& key,
                   const std::string& full_key, AxioConfig* config) {
  const toml::node& node = required_node(table, key, full_key, config);
  const auto value = node.value<double>();
  if (!value) {
    throw ConfigError(full_key, source_location(node, config->source_path),
                      "expected floating-point number");
  }
  return *value;
}

std::vector<uint32_t> read_u32_array(const toml::table& table,
                                     const std::string& key,
                                     const std::string& full_key,
                                     AxioConfig* config) {
  const toml::array& array = required_array(table, key, full_key, config);
  std::vector<uint32_t> values;
  values.reserve(array.size());
  for (const toml::node& node : array) {
    const auto value = node.value<int64_t>();
    if (!value || *value < 0 ||
        static_cast<uint64_t>(*value) > std::numeric_limits<uint32_t>::max()) {
      throw ConfigError(full_key, source_location(node, config->source_path),
                        "expected array of uint32 integers");
    }
    values.push_back(static_cast<uint32_t>(*value));
  }
  return values;
}

template <typename Enum>
Enum read_enum(const toml::table& table, const std::string& key,
               const std::string& full_key,
               std::initializer_list<std::pair<std::string_view, Enum>> values,
               AxioConfig* config) {
  const std::string value = read_string(table, key, full_key, config);
  for (const auto& [name, enum_value] : values) {
    if (name == value) {
      return enum_value;
    }
  }
  throw ConfigError(full_key, config->source_locations.at(full_key),
                    "unsupported value '" + value + "'");
}

PipelinePhase parse_pipeline_phase(const toml::node& node,
                                   const std::string& full_key,
                                   const AxioConfig& config) {
  const auto value = node.value<std::string>();
  if (!value) {
    throw ConfigError(full_key, source_location(node, config.source_path),
                      "expected array of phase strings");
  }
  if (*value == "app_tx") return PipelinePhase::kApplicationTx;
  if (*value == "dispatcher_tx") return PipelinePhase::kDispatcherTx;
  if (*value == "nic_tx") return PipelinePhase::kNicTx;
  if (*value == "nic_rx") return PipelinePhase::kNicRx;
  if (*value == "dispatcher_rx") return PipelinePhase::kDispatcherRx;
  if (*value == "app_rx") return PipelinePhase::kApplicationRx;
  throw ConfigError(full_key, source_location(node, config.source_path),
                    "unsupported pipeline phase '" + *value + "'");
}

AxioConfig parse_config(const toml::table& root,
                        const std::filesystem::path& path) {
  AxioConfig config;
  config.source_path = path;

  reject_unknown(root, "",
                 {"schema_version", "build", "runtime", "network", "metrics",
                  "deployment", "tuning", "workspaces", "workloads"},
                 path);

  config.schema_version =
      read_u32(root, "schema_version", "schema_version", &config);
  if (config.schema_version != 1) {
    throw ConfigError("schema_version",
                      config.source_locations.at("schema_version"),
                      "unsupported schema version " +
                          std::to_string(config.schema_version));
  }

  const toml::table& build = required_table(root, "build", "build", &config);
  reject_unknown(build, "build",
                 {"role", "backend", "roce_transport", "mtu",
                  "rx_ring_entries", "tx_ring_entries", "mempool_size",
                  "mempool_handler", "mempool_cache_size", "message_handler",
                  "packet_handler", "apply_new_mbuf", "request_payload_bytes",
                  "response_payload_bytes", "app_ticks_per_message",
                  "inflight_limit_enabled", "inflight_messages"},
                 path);
  config.build.role = read_enum<Role>(
      build, "role", "build.role",
      {{"client", Role::kClient}, {"server", Role::kServer}}, &config);
  config.build.backend = read_enum<Backend>(
      build, "backend", "build.backend",
      {{"dpdk", Backend::kDpdk}, {"roce", Backend::kRoce}}, &config);
  config.build.roce_transport = read_enum<RoceTransport>(
      build, "roce_transport", "build.roce_transport",
      {{"rc", RoceTransport::kRc}, {"ud", RoceTransport::kUd}}, &config);
  config.build.mtu = read_u32(build, "mtu", "build.mtu", &config);
  config.build.rx_ring_entries =
      read_u32(build, "rx_ring_entries", "build.rx_ring_entries", &config);
  config.build.tx_ring_entries =
      read_u32(build, "tx_ring_entries", "build.tx_ring_entries", &config);
  config.build.mempool_size =
      read_u32(build, "mempool_size", "build.mempool_size", &config);
  config.build.mempool_handler = read_enum<MempoolHandler>(
      build, "mempool_handler", "build.mempool_handler",
      {{"ring_mp_mc", MempoolHandler::kRingMpMc},
       {"ring_sp_sc", MempoolHandler::kRingSpSc},
       {"ring_mp_sc", MempoolHandler::kRingMpSc},
       {"ring_sp_mc", MempoolHandler::kRingSpMc},
       {"ring_mt_rts", MempoolHandler::kRingMtRts},
       {"ring_mt_hts", MempoolHandler::kRingMtHts},
       {"stack", MempoolHandler::kStack},
       {"lf_stack", MempoolHandler::kLfStack},
       {"bucket", MempoolHandler::kBucket},
       {"huge_alloc", MempoolHandler::kHugeAlloc}},
      &config);
  config.build.mempool_cache_size = read_u32(
      build, "mempool_cache_size", "build.mempool_cache_size", &config);
  config.build.message_handler = read_enum<MessageHandler>(
      build, "message_handler", "build.message_handler",
      {{"empty", MessageHandler::kEmpty},
       {"t_app", MessageHandler::kThroughput},
       {"l_app", MessageHandler::kLatency},
       {"m_app", MessageHandler::kMemory},
       {"file_write", MessageHandler::kFileWrite},
       {"file_read", MessageHandler::kFileRead},
       {"key_value", MessageHandler::kKeyValue}},
      &config);
  config.build.packet_handler = read_enum<PacketHandler>(
      build, "packet_handler", "build.packet_handler",
      {{"empty", PacketHandler::kEmpty}, {"echo", PacketHandler::kEcho}},
      &config);
  config.build.apply_new_mbuf =
      read_bool(build, "apply_new_mbuf", "build.apply_new_mbuf", &config);
  config.build.request_payload_bytes = read_u32(
      build, "request_payload_bytes", "build.request_payload_bytes", &config);
  config.build.response_payload_bytes = read_u32(
      build, "response_payload_bytes", "build.response_payload_bytes", &config);
  config.build.app_ticks_per_message = read_u64(
      build, "app_ticks_per_message", "build.app_ticks_per_message", &config);
  config.build.inflight_limit_enabled =
      read_bool(build, "inflight_limit_enabled",
                "build.inflight_limit_enabled", &config);
  config.build.inflight_messages = read_u64(
      build, "inflight_messages", "build.inflight_messages", &config);

  const toml::table& runtime =
      required_table(root, "runtime", "runtime", &config);
  reject_unknown(runtime, "runtime",
                 {"numa_node", "physical_port", "iterations", "window_seconds",
                  "app_tx_batch_size", "app_rx_batch_size",
                  "dispatcher_tx_batch_size", "dispatcher_rx_batch_size",
                  "nic_tx_post_size", "nic_rx_post_size"},
                 path);
  config.runtime.numa_node =
      read_u32(runtime, "numa_node", "runtime.numa_node", &config);
  config.runtime.physical_port =
      read_u32(runtime, "physical_port", "runtime.physical_port", &config);
  config.runtime.iterations =
      read_u32(runtime, "iterations", "runtime.iterations", &config);
  config.runtime.window_seconds =
      read_u32(runtime, "window_seconds", "runtime.window_seconds", &config);
  config.runtime.app_tx_batch_size = read_u32(
      runtime, "app_tx_batch_size", "runtime.app_tx_batch_size", &config);
  config.runtime.app_rx_batch_size = read_u32(
      runtime, "app_rx_batch_size", "runtime.app_rx_batch_size", &config);
  config.runtime.dispatcher_tx_batch_size =
      read_u32(runtime, "dispatcher_tx_batch_size",
               "runtime.dispatcher_tx_batch_size", &config);
  config.runtime.dispatcher_rx_batch_size =
      read_u32(runtime, "dispatcher_rx_batch_size",
               "runtime.dispatcher_rx_batch_size", &config);
  config.runtime.nic_tx_post_size = read_u32(
      runtime, "nic_tx_post_size", "runtime.nic_tx_post_size", &config);
  config.runtime.nic_rx_post_size = read_u32(
      runtime, "nic_rx_post_size", "runtime.nic_rx_post_size", &config);

  const toml::table& network =
      required_table(root, "network", "network", &config);
  reject_unknown(network, "network",
                 {"local_ip", "remote_ip", "local_mac", "remote_mac",
                  "device_pcie", "device_name"},
                 path);
  config.network.local_ip =
      read_string(network, "local_ip", "network.local_ip", &config);
  config.network.remote_ip =
      read_string(network, "remote_ip", "network.remote_ip", &config);
  config.network.local_mac =
      read_string(network, "local_mac", "network.local_mac", &config);
  config.network.remote_mac =
      read_string(network, "remote_mac", "network.remote_mac", &config);
  config.network.device_pcie =
      read_string(network, "device_pcie", "network.device_pcie", &config);
  config.network.device_name =
      read_string(network, "device_name", "network.device_name", &config);

  const toml::table& metrics =
      required_table(root, "metrics", "metrics", &config);
  reject_unknown(metrics, "metrics", {"jsonl_path", "human_output"}, path);
  config.metrics.jsonl_path =
      read_string(metrics, "jsonl_path", "metrics.jsonl_path", &config);
  config.metrics.human_output =
      read_bool(metrics, "human_output", "metrics.human_output", &config);

  const toml::table& deployment =
      required_table(root, "deployment", "deployment", &config);
  reject_unknown(deployment, "deployment",
                 {"host", "ssh_port", "ssh_user", "workdir", "use_sudo"},
                 path);
  config.deployment.host =
      read_string(deployment, "host", "deployment.host", &config);
  config.deployment.ssh_port =
      read_u32(deployment, "ssh_port", "deployment.ssh_port", &config);
  config.deployment.ssh_user =
      read_string(deployment, "ssh_user", "deployment.ssh_user", &config);
  config.deployment.workdir =
      read_string(deployment, "workdir", "deployment.workdir", &config);
  config.deployment.use_sudo =
      read_bool(deployment, "use_sudo", "deployment.use_sudo", &config);

  const toml::table& tuning =
      required_table(root, "tuning", "tuning", &config);
  reject_unknown(tuning, "tuning",
                 {"max_iterations", "latency_slo_us", "warmup_windows",
                  "sample_windows", "infrastructure_failure_limit", "noise",
                  "resources"},
                 path);
  config.tuning.max_iterations =
      read_u32(tuning, "max_iterations", "tuning.max_iterations", &config);
  config.tuning.latency_slo_us =
      read_double(tuning, "latency_slo_us", "tuning.latency_slo_us", &config);
  config.tuning.warmup_windows =
      read_u32(tuning, "warmup_windows", "tuning.warmup_windows", &config);
  config.tuning.sample_windows =
      read_u32(tuning, "sample_windows", "tuning.sample_windows", &config);
  config.tuning.infrastructure_failure_limit =
      read_u32(tuning, "infrastructure_failure_limit",
               "tuning.infrastructure_failure_limit", &config);

  const toml::table& noise =
      required_table(tuning, "noise", "tuning.noise", &config);
  reject_unknown(noise, "tuning.noise",
                 {"throughput_relative_floor", "latency_relative_floor",
                  "stage_time_relative_floor", "stall_time_relative_floor",
                  "miss_rate_percentage_point_floor"},
                 path);
  config.tuning.noise.throughput_relative_floor =
      read_double(noise, "throughput_relative_floor",
                  "tuning.noise.throughput_relative_floor", &config);
  config.tuning.noise.latency_relative_floor =
      read_double(noise, "latency_relative_floor",
                  "tuning.noise.latency_relative_floor", &config);
  config.tuning.noise.stage_time_relative_floor =
      read_double(noise, "stage_time_relative_floor",
                  "tuning.noise.stage_time_relative_floor", &config);
  config.tuning.noise.stall_time_relative_floor =
      read_double(noise, "stall_time_relative_floor",
                  "tuning.noise.stall_time_relative_floor", &config);
  config.tuning.noise.miss_rate_percentage_point_floor =
      read_double(noise, "miss_rate_percentage_point_floor",
                  "tuning.noise.miss_rate_percentage_point_floor", &config);

  const toml::table& resources =
      required_table(tuning, "resources", "tuning.resources", &config);
  reject_unknown(resources, "tuning.resources",
                 {"application_workspaces", "dispatcher_workspaces"}, path);
  config.tuning.resources.application_workspaces = read_u32_array(
      resources, "application_workspaces",
      "tuning.resources.application_workspaces", &config);
  config.tuning.resources.dispatcher_workspaces = read_u32_array(
      resources, "dispatcher_workspaces",
      "tuning.resources.dispatcher_workspaces", &config);

  const toml::array& workspaces =
      required_array(root, "workspaces", "workspaces", &config);
  config.workspaces.reserve(workspaces.size());
  size_t workspace_index = 0;
  for (const toml::node& node : workspaces) {
    const toml::table* workspace = node.as_table();
    const std::string prefix =
        "workspaces[" + std::to_string(workspace_index) + "]";
    if (workspace == nullptr) {
      throw ConfigError(prefix, source_location(node, path), "expected table");
    }
    reject_unknown(*workspace, prefix, {"id", "cpu_core"}, path);
    WorkspaceConfig value;
    value.id = read_u32(*workspace, "id", prefix + ".id", &config);
    value.cpu_core =
        read_u32(*workspace, "cpu_core", prefix + ".cpu_core", &config);
    config.workspaces.push_back(value);
    ++workspace_index;
  }

  const toml::array& workloads =
      required_array(root, "workloads", "workloads", &config);
  config.workloads.reserve(workloads.size());
  size_t workload_index = 0;
  for (const toml::node& node : workloads) {
    const toml::table* workload = node.as_table();
    const std::string prefix =
        "workloads[" + std::to_string(workload_index) + "]";
    if (workload == nullptr) {
      throw ConfigError(prefix, source_location(node, path), "expected table");
    }
    reject_unknown(*workload, prefix,
                   {"id", "pipeline", "remote_dispatchers", "groups"}, path);
    WorkloadConfig value;
    value.id = read_u32(*workload, "id", prefix + ".id", &config);
    const toml::array& pipeline =
        required_array(*workload, "pipeline", prefix + ".pipeline", &config);
    value.pipeline.reserve(pipeline.size());
    for (const toml::node& phase : pipeline) {
      value.pipeline.push_back(
          parse_pipeline_phase(phase, prefix + ".pipeline", config));
    }
    value.remote_dispatchers = read_u32_array(
        *workload, "remote_dispatchers", prefix + ".remote_dispatchers",
        &config);

    const toml::array& groups =
        required_array(*workload, "groups", prefix + ".groups", &config);
    size_t group_index = 0;
    for (const toml::node& group_node : groups) {
      const toml::table* group = group_node.as_table();
      const std::string group_prefix =
          prefix + ".groups[" + std::to_string(group_index) + "]";
      if (group == nullptr) {
        throw ConfigError(group_prefix, source_location(group_node, path),
                          "expected table");
      }
      reject_unknown(*group, group_prefix, {"dispatcher", "applications"},
                     path);
      WorkloadGroupConfig group_value;
      group_value.dispatcher =
          read_u32(*group, "dispatcher", group_prefix + ".dispatcher", &config);
      group_value.applications = read_u32_array(
          *group, "applications", group_prefix + ".applications", &config);
      value.groups.push_back(std::move(group_value));
      ++group_index;
    }
    config.workloads.push_back(std::move(value));
    ++workload_index;
  }

  return config;
}

}  // namespace

std::string SourceLocation::format() const {
  return this->path + ":" + std::to_string(this->line) + ":" +
         std::to_string(this->column);
}

ConfigError::ConfigError(std::string key, SourceLocation source,
                         std::string message)
    : std::runtime_error(diagnostic(key, source, message)),
      key_(std::move(key)),
      source_(std::move(source)) {}

AxioConfig load_config(const std::filesystem::path& path) {
  try {
    const toml::table root = toml::parse_file(path.string());
    return parse_config(root, path);
  } catch (const toml::parse_error& error) {
    throw ConfigError("<toml>", source_location(error.source(), path),
                      std::string(error.description()));
  }
}

}  // namespace axio::config
