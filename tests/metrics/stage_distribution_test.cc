#include "metrics/stage_distribution.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void expect_near(double actual, double expected, const std::string& message) {
  if (std::fabs(actual - expected) > 1e-9) {
    throw std::runtime_error(message);
  }
}

void test_stride_and_ring_wrap() {
  axio::metrics::StageDistributionSampler sampler(2, 4);
  for (uint64_t value = 1; value <= 12; ++value) sampler.record(value);
  const std::vector<uint64_t> samples = sampler.samples();
  expect(samples.size() == 4, "sampler must retain its bounded capacity");
  expect(samples == std::vector<uint64_t>({6, 8, 10, 12}),
         "sampler must retain the most recent strided observations");
}

void test_true_percentiles() {
  const axio::metrics::DistributionSummary summary =
      axio::metrics::summarize_distribution({1, 2, 3, 4, 100});
  expect(summary.available, "non-empty samples must be available");
  expect(summary.sample_count == 5, "sample count must be preserved");
  expect_near(summary.p1, 1, "P1 must use the sorted empirical sample");
  expect_near(summary.p50, 3, "P50 must use the sorted empirical sample");
  expect_near(summary.p99, 100, "P99 must use the sorted empirical sample");
  expect_near(summary.mean, 22, "mean must use all retained samples");
  expect_near(summary.minimum, 1, "minimum must be reported");
  expect_near(summary.maximum, 100, "maximum must be reported");

  const axio::metrics::DistributionSummary empty =
      axio::metrics::summarize_distribution({});
  expect(!empty.available && empty.sample_count == 0,
         "an empty window must remain explicitly unavailable");
}

void test_jsonl_writer() {
  const std::filesystem::path output =
      std::filesystem::temp_directory_path() /
      "axio-stage-distribution-test.jsonl";
  {
    axio::metrics::StageDistributionWriter writer(output, true);
    axio::metrics::StageDistributionRecord record;
    record.window_id = 7;
    record.sample_stride = 64;
    record.app_tx_allocation_stall =
        axio::metrics::summarize_distribution({1.25, 2.75});
    record.app_rx_handler_completion =
        axio::metrics::summarize_distribution({3.5});
    writer.append(record);
  }
  std::ifstream input(output);
  std::string line;
  std::getline(input, line);
  expect(line.find("\"schema\":\"axio.stage-distribution/v1\"") !=
             std::string::npos,
         "writer must identify the independent distribution schema");
  expect(line.find("\"window_id\":7") != std::string::npos,
         "writer must preserve the metrics window ID");
  expect(line.find("\"p50_us\":1.25") != std::string::npos,
         "writer must serialize true percentiles");
  std::error_code error;
  std::filesystem::remove(output, error);
}

}  // namespace

int main() {
  test_stride_and_ring_wrap();
  test_true_percentiles();
  test_jsonl_writer();
  return 0;
}
