/**
 * @file config_types.h
 * @brief Typed representation of Axio TOML schema v1.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace axio::config {

struct SourceLocation {
  std::string path;
  uint32_t line = 1;
  uint32_t column = 1;

  std::string format() const;
};

enum class Role : uint8_t {
  kClient,
  kServer,
};

enum class Backend : uint8_t {
  kDpdk,
  kRoce,
};

enum class RoceTransport : uint8_t {
  kRc,
  kUd,
};

enum class MempoolHandler : uint8_t {
  kRingMpMc,
  kRingSpSc,
  kRingMpSc,
  kRingSpMc,
  kRingMtRts,
  kRingMtHts,
  kStack,
  kLfStack,
  kBucket,
  kHugeAlloc,
};

enum class MessageHandler : uint8_t {
  kEmpty,
  kThroughput,
  kLatency,
  kMemory,
  kFileWrite,
  kFileRead,
  kKeyValue,
};

enum class PacketHandler : uint8_t {
  kEmpty,
  kEcho,
};

enum class PipelinePhase : uint8_t {
  kApplicationTx,
  kDispatcherTx,
  kNicTx,
  kNicRx,
  kDispatcherRx,
  kApplicationRx,
};

struct NetworkConfig {
  Backend backend = Backend::kDpdk;
  RoceTransport roce_transport = RoceTransport::kRc;
  uint32_t physical_port = 0;
  uint32_t rx_ring_entries = 0;
  uint32_t tx_ring_entries = 0;
  std::string local_ip;
  std::string remote_ip;
  std::string local_mac;
  std::string remote_mac;
  std::string device_pcie;
  std::string device_name;
};

struct HandlerConfig {
  MessageHandler message_handler = MessageHandler::kEmpty;
  PacketHandler packet_handler = PacketHandler::kEmpty;
  bool apply_new_mbuf = false;
  uint32_t request_payload_bytes = 0;
  uint32_t response_payload_bytes = 0;
  uint64_t app_ticks_per_message = 0;
};

struct BuildKnobsConfig {
  bool inflight_limit_enabled = false;
  uint64_t inflight_messages = 0;
  uint32_t mtu = 0;
  MempoolHandler mempool_handler = MempoolHandler::kRingMpMc;
};

struct RuntimeKnobsConfig {
  uint32_t application_core_count = 0;
  uint32_t dispatcher_queue_count = 0;
  uint32_t app_tx_batch_size = 0;
  uint32_t app_rx_batch_size = 0;
  uint32_t dispatcher_tx_batch_size = 0;
  uint32_t dispatcher_rx_batch_size = 0;
  uint32_t nic_tx_post_size = 0;
  uint32_t nic_rx_post_size = 0;
};

struct KnobsConfig {
  BuildKnobsConfig build;
  RuntimeKnobsConfig runtime;
};

struct OtherConfig {
  uint32_t iterations = 0;
  uint32_t window_seconds = 0;
  uint32_t mempool_size = 0;
  uint32_t mempool_cache_size = 0;
};

struct MetricsConfig {
  std::filesystem::path jsonl_path;
  bool human_output = true;
};

struct TuningNoiseConfig {
  double throughput_relative_floor = 0.0;
  double latency_relative_floor = 0.0;
  double stage_time_relative_floor = 0.0;
  double stall_time_relative_floor = 0.0;
  double miss_rate_percentage_point_floor = 0.0;
};

struct TuningConfig {
  uint32_t max_iterations = 0;
  double latency_slo_us = 0.0;
  uint32_t warmup_windows = 0;
  uint32_t sample_windows = 0;
  uint32_t infrastructure_failure_limit = 0;
  TuningNoiseConfig noise;
};

struct WorkspaceConfig {
  uint32_t id = 0;
  // Zero-based CPU-core ordinal within deployment.numa_node.
  uint32_t cpu_core = 0;
};

struct WorkloadGroupConfig {
  uint32_t dispatcher = 0;
  std::vector<uint32_t> applications;
};

struct WorkloadConfig {
  uint32_t id = 0;
  std::vector<PipelinePhase> pipeline;
  std::vector<uint32_t> remote_dispatchers;
  std::vector<WorkloadGroupConfig> groups;
};

struct DeploymentTopologyConfig {
  std::vector<uint32_t> application_workspaces;
  std::vector<uint32_t> dispatcher_workspaces;
  std::vector<WorkspaceConfig> workspaces;
  std::vector<WorkloadConfig> workloads;
};

struct DeploymentConfig {
  Role role = Role::kServer;
  uint32_t numa_node = 0;
  std::string host;
  uint32_t ssh_port = 22;
  std::string ssh_user;
  std::filesystem::path workdir;
  bool use_sudo = false;
  DeploymentTopologyConfig topology;
};

struct AxioConfig {
  uint32_t schema_version = 0;
  DeploymentConfig deployment;
  NetworkConfig network;
  HandlerConfig handler;
  KnobsConfig knobs;
  OtherConfig other;
  MetricsConfig metrics;
  std::optional<TuningConfig> tuning;
  std::filesystem::path source_path;
  std::map<std::string, SourceLocation> source_locations;
};

}  // namespace axio::config
