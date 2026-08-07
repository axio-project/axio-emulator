/**
 * @file metrics_record.h
 * @brief Typed, immutable-at-publication Axio measurement-window data.
 */
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifndef AXIO_VERSION
#define AXIO_VERSION "unknown"
#endif

#ifndef AXIO_GIT_COMMIT
#define AXIO_GIT_COMMIT "unknown"
#endif

namespace axio::metrics {

inline constexpr std::string_view kMetricsSchema = "axio.metrics/v1";

inline std::string_view build_version() { return AXIO_VERSION; }
inline std::string_view build_git_commit() { return AXIO_GIT_COMMIT; }

inline std::string make_run_id(uint64_t process_id,
                               uint64_t monotonic_start_ns) {
  return "pid-" + std::to_string(process_id) + "-monotonic-ns-" +
         std::to_string(monotonic_start_ns);
}

struct MetricsRunMetadata {
  std::string run_id;
  std::string role;
  std::string backend;
  std::string version = std::string(build_version());
  std::string git_commit = std::string(build_git_commit());
  std::string build_fingerprint;
  std::string config_fingerprint;
};

struct MetricValue {
  bool available = false;
  double value = 0;

  static MetricValue unavailable() { return {}; }
  static MetricValue from_value(double value) { return {true, value}; }
};

struct StageMetricsRecord {
  MetricValue throughput_mpps;
  MetricValue completion_time_per_packet_us;
  MetricValue stall_time_per_packet_us;
};

struct QueueMetricsRecord {
  uint32_t workspace_id = 0;
  std::vector<uint32_t> workload_ids;
  uint64_t successful_completion_count = 0;
  uint64_t timed_completion_count = 0;
  uint64_t successful_poll_count = 0;
  uint64_t empty_poll_count = 0;
  uint64_t completion_error_count = 0;
  uint64_t first_completion_tsc = 0;
  uint64_t last_completion_tsc = 0;
  double tsc_frequency_ghz = 0;
  bool clock_valid = false;
  bool measurement_valid = false;
  bool capacity_comparable = false;
  std::vector<std::string> invalid_reasons;
  MetricValue completion_interval_cycles;
  MetricValue completion_interval_ns;
  MetricValue completion_rate_mpps;
};

struct MetricsRecord {
  std::string schema = std::string(kMetricsSchema);
  std::string run_id;
  uint64_t window_id = 0;
  std::string role;
  std::string backend;
  std::string version = std::string(build_version());
  std::string git_commit = std::string(build_git_commit());
  std::string build_fingerprint;
  std::string config_fingerprint;
  double duration_seconds = 0;
  bool measurement_valid = false;
  bool capacity_comparable = false;
  std::vector<std::string> invalid_reasons;

  MetricValue e2e_throughput_mpps;
  MetricValue latency_p50_us;
  MetricValue latency_p99_us;
  MetricValue latency_p999_us;

  StageMetricsRecord app_tx;
  StageMetricsRecord app_rx;
  StageMetricsRecord dispatcher_tx;
  StageMetricsRecord dispatcher_rx;
  MetricValue nic_tx_throughput_mpps;
  MetricValue nic_tx_submit_time_per_packet_us;
  MetricValue nic_rx_throughput_mpps;
  MetricValue nic_rx_completion_interval_cycles;
  MetricValue nic_rx_completion_interval_ns;
  MetricValue nic_rx_slowest_interval_cycles;
  MetricValue nic_rx_capacity_interval_cycles;

  uint64_t app_enqueue_drop_count = 0;
  uint64_t dispatcher_enqueue_drop_count = 0;
  uint64_t nic_tx_packet_count = 0;
  uint64_t nic_rx_successful_completion_count = 0;
  uint64_t nic_rx_timed_completion_count = 0;
  uint64_t nic_rx_completion_error_count = 0;
  std::vector<QueueMetricsRecord> queues;
};

}  // namespace axio::metrics
