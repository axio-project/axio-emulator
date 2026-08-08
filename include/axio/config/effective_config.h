/**
 * @file effective_config.h
 * @brief Canonical fingerprint of one complete Axio datapath configuration.
 */
#pragma once

#include "axio/config/build_config.h"

#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace axio::config {
namespace detail {

inline void append_effective_field(std::ostringstream* output,
                                   std::string_view key,
                                   std::string_view value) {
  *output << key.size() << ':' << key << '=' << value.size() << ':' << value
          << '\n';
}

template <typename T>
inline void append_effective_scalar(std::ostringstream* output,
                                    std::string_view key, T value) {
  std::ostringstream encoded;
  encoded << std::boolalpha << std::setprecision(
      std::numeric_limits<double>::max_digits10) << value;
  append_effective_field(output, key, encoded.str());
}

}  // namespace detail

inline std::string canonical_effective_config(const AxioConfig& config) {
  std::ostringstream output;
  const auto field = [&](std::string_view key, const auto& value) {
    detail::append_effective_scalar(&output, key, value);
  };
  const auto text = [&](std::string_view key, std::string_view value) {
    detail::append_effective_field(&output, key, value);
  };

  field("schema_version", config.schema_version);
  text("deployment.role", to_string(config.deployment.role));
  field("deployment.numa_node", config.deployment.numa_node);

  text("network.backend", to_string(config.network.backend));
  text("network.roce_transport", to_string(config.network.roce_transport));
  field("network.physical_port", config.network.physical_port);
  field("network.rx_ring_entries", config.network.rx_ring_entries);
  field("network.tx_ring_entries", config.network.tx_ring_entries);
  text("network.local_ip", config.network.local_ip);
  text("network.remote_ip", config.network.remote_ip);
  text("network.local_mac", config.network.local_mac);
  text("network.remote_mac", config.network.remote_mac);
  text("network.device_pcie", config.network.device_pcie);
  text("network.device_name", config.network.device_name);

  text("handler.message_handler", to_string(config.handler.message_handler));
  text("handler.packet_handler", to_string(config.handler.packet_handler));
  field("handler.apply_new_mbuf", config.handler.apply_new_mbuf);
  field("handler.request_payload_bytes", config.handler.request_payload_bytes);
  field("handler.response_payload_bytes",
        config.handler.response_payload_bytes);
  field("handler.app_ticks_per_message",
        config.handler.app_ticks_per_message);

  field("knobs.build.inflight_limit_enabled",
        config.knobs.build.inflight_limit_enabled);
  field("knobs.build.inflight_messages",
        config.knobs.build.inflight_messages);
  field("knobs.build.mtu", config.knobs.build.mtu);
  text("knobs.build.mempool_handler",
       to_string(config.knobs.build.mempool_handler));
  field("knobs.runtime.application_core_count",
        config.knobs.runtime.application_core_count);
  field("knobs.runtime.dispatcher_queue_count",
        config.knobs.runtime.dispatcher_queue_count);
  field("knobs.runtime.app_tx_batch_size",
        config.knobs.runtime.app_tx_batch_size);
  field("knobs.runtime.app_rx_batch_size",
        config.knobs.runtime.app_rx_batch_size);
  field("knobs.runtime.dispatcher_tx_batch_size",
        config.knobs.runtime.dispatcher_tx_batch_size);
  field("knobs.runtime.dispatcher_rx_batch_size",
        config.knobs.runtime.dispatcher_rx_batch_size);
  field("knobs.runtime.nic_tx_post_size",
        config.knobs.runtime.nic_tx_post_size);
  field("knobs.runtime.nic_rx_post_size",
        config.knobs.runtime.nic_rx_post_size);

  field("other.iterations", config.other.iterations);
  field("other.window_seconds", config.other.window_seconds);
  field("other.mempool_size", config.other.mempool_size);
  field("other.mempool_cache_size", config.other.mempool_cache_size);
  field("metrics.enabled", config.metrics.enabled);
  field("metrics.human_output", config.metrics.human_output);

  field("tuning.present", config.tuning.has_value());
  if (config.tuning.has_value()) {
    const TuningConfig& tuning = *config.tuning;
    field("tuning.max_iterations", tuning.max_iterations);
    field("tuning.latency_slo_us", tuning.latency_slo_us);
    field("tuning.warmup_windows", tuning.warmup_windows);
    field("tuning.sample_windows", tuning.sample_windows);
    field("tuning.infrastructure_failure_limit",
          tuning.infrastructure_failure_limit);
    field("tuning.noise.throughput_relative_floor",
          tuning.noise.throughput_relative_floor);
    field("tuning.noise.latency_relative_floor",
          tuning.noise.latency_relative_floor);
    field("tuning.noise.stage_time_relative_floor",
          tuning.noise.stage_time_relative_floor);
    field("tuning.noise.stall_time_relative_floor",
          tuning.noise.stall_time_relative_floor);
    field("tuning.noise.miss_rate_percentage_point_floor",
          tuning.noise.miss_rate_percentage_point_floor);
  }

  const DeploymentTopologyConfig& topology = config.deployment.topology;
  field("topology.application_workspaces.count",
        topology.application_workspaces.size());
  for (size_t index = 0; index < topology.application_workspaces.size();
       ++index) {
    field("topology.application_workspaces[" + std::to_string(index) + "]",
          topology.application_workspaces[index]);
  }
  field("topology.dispatcher_workspaces.count",
        topology.dispatcher_workspaces.size());
  for (size_t index = 0; index < topology.dispatcher_workspaces.size();
       ++index) {
    field("topology.dispatcher_workspaces[" + std::to_string(index) + "]",
          topology.dispatcher_workspaces[index]);
  }
  field("topology.workspaces.count", topology.workspaces.size());
  for (size_t index = 0; index < topology.workspaces.size(); ++index) {
    const WorkspaceConfig& workspace = topology.workspaces[index];
    const std::string prefix =
        "topology.workspaces[" + std::to_string(index) + "]";
    field(prefix + ".id", workspace.id);
    field(prefix + ".cpu_core", workspace.cpu_core);
  }
  field("topology.workloads.count", topology.workloads.size());
  for (size_t workload_index = 0;
       workload_index < topology.workloads.size(); ++workload_index) {
    const WorkloadConfig& workload = topology.workloads[workload_index];
    const std::string prefix =
        "topology.workloads[" + std::to_string(workload_index) + "]";
    field(prefix + ".id", workload.id);
    field(prefix + ".pipeline.count", workload.pipeline.size());
    for (size_t index = 0; index < workload.pipeline.size(); ++index) {
      text(prefix + ".pipeline[" + std::to_string(index) + "]",
           to_string(workload.pipeline[index]));
    }
    field(prefix + ".remote_dispatchers.count",
          workload.remote_dispatchers.size());
    for (size_t index = 0; index < workload.remote_dispatchers.size();
         ++index) {
      field(prefix + ".remote_dispatchers[" + std::to_string(index) + "]",
            workload.remote_dispatchers[index]);
    }
    field(prefix + ".groups.count", workload.groups.size());
    for (size_t group_index = 0; group_index < workload.groups.size();
         ++group_index) {
      const WorkloadGroupConfig& group = workload.groups[group_index];
      const std::string group_prefix =
          prefix + ".groups[" + std::to_string(group_index) + "]";
      field(group_prefix + ".dispatcher", group.dispatcher);
      field(group_prefix + ".applications.count", group.applications.size());
      for (size_t index = 0; index < group.applications.size(); ++index) {
        field(group_prefix + ".applications[" + std::to_string(index) + "]",
              group.applications[index]);
      }
    }
  }
  return output.str();
}

inline std::string effective_config_fingerprint(const AxioConfig& config) {
  return fingerprint_text(canonical_effective_config(config));
}

inline std::string canonical_deployment_config(const AxioConfig& config) {
  std::ostringstream output;
  detail::append_effective_field(&output, "deployment.transport",
                                 to_string(config.deployment.transport));
  detail::append_effective_field(&output, "deployment.role",
                                 to_string(config.deployment.role));
  detail::append_effective_scalar(&output, "deployment.numa_node",
                                  config.deployment.numa_node);
  detail::append_effective_field(&output, "deployment.host",
                                 config.deployment.host);
  detail::append_effective_scalar(&output, "deployment.ssh_port",
                                  config.deployment.ssh_port);
  detail::append_effective_field(&output, "deployment.ssh_user",
                                 config.deployment.ssh_user);
  detail::append_effective_field(&output, "deployment.workdir",
                                 config.deployment.workdir.string());
  detail::append_effective_scalar(&output, "deployment.use_sudo",
                                  config.deployment.use_sudo);
  return output.str();
}

inline std::string deployment_fingerprint(const AxioConfig& config) {
  return fingerprint_text(canonical_deployment_config(config));
}

}  // namespace axio::config
