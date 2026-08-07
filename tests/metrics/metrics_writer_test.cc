#include "metrics/metrics_writer.h"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace metrics = axio::metrics;

namespace {

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::string read_file(const fs::path& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    throw std::runtime_error("failed to read " + path.string());
  }
  std::ostringstream output;
  output << input.rdbuf();
  return output.str();
}

class TempDirectory {
 public:
  TempDirectory()
      : path_(fs::temp_directory_path() /
              ("axio-metrics-writer-" + std::to_string(getpid()))) {
    std::error_code error;
    fs::remove_all(this->path_, error);
    fs::create_directories(this->path_);
  }

  ~TempDirectory() {
    std::error_code error;
    fs::remove_all(this->path_, error);
  }

  const fs::path& path() const { return this->path_; }

 private:
  fs::path path_;
};

metrics::MetricsRecord make_record() {
  metrics::MetricsRecord record;
  record.run_id = "run\"\\\nidentifier";
  record.window_id = 3;
  record.role = "server";
  record.backend = "dpdk";
  record.version = "1.2.0-test";
  record.git_commit = "0123456789abcdef";
  record.build_fingerprint = "fnv1a64:build";
  record.config_fingerprint = "fnv1a64:config";
  record.duration_seconds = 1.0;
  record.measurement_valid = true;
  record.capacity_comparable = false;
  record.invalid_reasons = {"offered load below capacity"};
  record.e2e_throughput_mpps = metrics::MetricValue::from_value(45.125);
  record.latency_p50_us = metrics::MetricValue::unavailable();
  record.latency_p99_us = metrics::MetricValue::from_value(2.25);
  record.latency_p999_us = metrics::MetricValue::from_value(2.75);
  record.app_tx.throughput_mpps = metrics::MetricValue::from_value(45.125);
  record.app_tx.completion_time_per_packet_us =
      metrics::MetricValue::from_value(0.01);
  record.app_tx.stall_time_per_packet_us = metrics::MetricValue::from_value(0.02);
  record.nic_tx_throughput_mpps = metrics::MetricValue::from_value(45.125);
  record.nic_tx_submit_time_per_packet_us =
      metrics::MetricValue::from_value(0.03);
  record.nic_rx_throughput_mpps = metrics::MetricValue::from_value(45.125);
  record.nic_rx_completion_interval_cycles =
      metrics::MetricValue::from_value(42.0);
  record.nic_rx_completion_interval_ns =
      metrics::MetricValue::from_value(14.0);
  record.nic_rx_slowest_interval_cycles =
      metrics::MetricValue::from_value(48.0);
  record.nic_rx_capacity_interval_cycles =
      metrics::MetricValue::from_value(21.0);
  record.nic_rx_successful_completion_count = 45125000;
  record.nic_rx_completion_error_count = 0;

  metrics::QueueMetricsRecord valid_queue;
  valid_queue.workspace_id = 0;
  valid_queue.workload_ids = {1};
  valid_queue.successful_completion_count = 100;
  valid_queue.timed_completion_count = 96;
  valid_queue.successful_poll_count = 20;
  valid_queue.empty_poll_count = 4;
  valid_queue.first_completion_tsc = 1000;
  valid_queue.last_completion_tsc = 5000;
  valid_queue.tsc_frequency_ghz = 3.0;
  valid_queue.clock_valid = true;
  valid_queue.measurement_valid = true;
  valid_queue.completion_interval_cycles = metrics::MetricValue::from_value(41.0);
  valid_queue.completion_interval_ns = metrics::MetricValue::from_value(13.666);
  valid_queue.completion_rate_mpps = metrics::MetricValue::from_value(73.17);
  record.queues.push_back(valid_queue);

  metrics::QueueMetricsRecord invalid_queue;
  invalid_queue.workspace_id = 1;
  invalid_queue.workload_ids = {1, 2};
  invalid_queue.clock_valid = false;
  invalid_queue.measurement_valid = false;
  invalid_queue.invalid_reasons = {"cpu migration"};
  record.queues.push_back(invalid_queue);
  return record;
}

void test_writer_creates_parent_truncates_and_appends_complete_lines() {
  TempDirectory temp;
  const fs::path output = temp.path() / "nested" / "metrics.jsonl";
  fs::create_directories(output.parent_path());
  {
    std::ofstream stale(output);
    stale << "stale\n";
  }

  metrics::MetricsWriter writer(output, true);
  writer.append(make_record());
  const std::string first = read_file(output);
  expect(first.find("stale") == std::string::npos,
         "writer must truncate stale output at run start");
  expect(std::count(first.begin(), first.end(), '\n') == 1,
         "one append must produce exactly one JSONL record");
  expect(first.back() == '\n', "JSONL record must end in one newline");
  expect(first.find("\"schema\":\"axio.metrics/v1\"") !=
             std::string::npos,
         "writer must emit the public schema identifier");
  expect(first.find("run\\\"\\\\\\nidentifier") != std::string::npos,
         "writer must JSON-escape identity strings");
  expect(first.find("\"p50_us\":null") != std::string::npos,
         "unavailable metrics must be JSON null");
  expect(first.find("\"e2e_mpps\":45.125") != std::string::npos,
         "available metrics must retain numeric values");
  expect(first.find("\"completion_interval_cycles\":null") !=
             std::string::npos,
         "invalid queue metrics must remain null");

  metrics::MetricsRecord second = make_record();
  second.window_id = 4;
  writer.append(second);
  const std::string both = read_file(output);
  expect(std::count(both.begin(), both.end(), '\n') == 2,
         "sequential appends must produce one line per window");
  expect(both.find("\"window_id\":3") < both.find("\"window_id\":4"),
         "append order must preserve window order");

  const fs::path created_output =
      temp.path() / "writer-created" / "nested" / "metrics.jsonl";
  metrics::MetricsWriter parent_writer(created_output, true);
  parent_writer.append(make_record());
  expect(fs::is_regular_file(created_output),
         "writer must create a missing parent directory");
}

void test_disabled_writer_has_no_filesystem_side_effect() {
  TempDirectory temp;
  const fs::path output = temp.path() / "disabled" / "metrics.jsonl";
  metrics::MetricsWriter writer(output, false);
  writer.append(make_record());
  expect(!fs::exists(output), "disabled writer must not create an output file");
}

void test_open_and_nonfinite_failures_are_reported() {
  TempDirectory temp;
  bool open_failed = false;
  try {
    metrics::MetricsWriter writer(temp.path(), true);
  } catch (const std::exception&) {
    open_failed = true;
  }
  expect(open_failed, "opening a directory as JSONL must fail");

  const fs::path output = temp.path() / "finite.jsonl";
  metrics::MetricsWriter writer(output, true);
  metrics::MetricsRecord record = make_record();
  record.e2e_throughput_mpps = metrics::MetricValue::from_value(
      std::numeric_limits<double>::infinity());
  bool finite_failed = false;
  try {
    writer.append(record);
  } catch (const std::exception&) {
    finite_failed = true;
  }
  expect(finite_failed, "writer must reject a non-finite available metric");
  expect(read_file(output).empty(),
         "invalid records must be rejected before partial output");
}

void test_flush_failure_is_reported_when_platform_exposes_dev_full() {
  if (!fs::exists("/dev/full")) return;
  metrics::MetricsWriter writer("/dev/full", true);
  bool failed = false;
  try {
    writer.append(make_record());
  } catch (const std::exception&) {
    failed = true;
  }
  expect(failed, "writer must report write or flush failure");
}

}  // namespace

int main() {
  try {
    test_writer_creates_parent_truncates_and_appends_complete_lines();
    test_disabled_writer_has_no_filesystem_side_effect();
    test_open_and_nonfinite_failures_are_reported();
    test_flush_failure_is_reported_when_platform_exposes_dev_full();
    std::cout << "Axio metrics writer test passed" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Axio metrics writer test failed: " << error.what()
              << std::endl;
    return 1;
  }
}
