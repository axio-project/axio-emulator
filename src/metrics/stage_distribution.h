/**
 * @file stage_distribution.h
 * @brief Bounded AE-only stage sampling and JSONL publication.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace axio::metrics {

inline constexpr const char* kStageDistributionSchema =
    "axio.stage-distribution/v1";

class StageDistributionSampler {
 public:
  StageDistributionSampler(size_t sample_stride, size_t sample_capacity);

  void record(uint64_t cycles);
  std::vector<uint64_t> samples() const;
  std::vector<uint64_t> take_samples();

 private:
  size_t sample_stride_;
  size_t sample_capacity_;
  size_t observation_count_ = 0;
  size_t next_index_ = 0;
  bool wrapped_ = false;
  std::vector<uint64_t> samples_;
};

struct DistributionSummary {
  bool available = false;
  size_t sample_count = 0;
  double p1 = 0.0;
  double p50 = 0.0;
  double p99 = 0.0;
  double mean = 0.0;
  double minimum = 0.0;
  double maximum = 0.0;
};

DistributionSummary summarize_distribution(std::vector<double> samples);

struct StageDistributionRecord {
  uint64_t window_id = 0;
  uint32_t sample_stride = 0;
  DistributionSummary app_tx_allocation_stall;
  DistributionSummary app_rx_handler_completion;
};

class StageDistributionWriter {
 public:
  StageDistributionWriter(std::filesystem::path output_path, bool enabled);

  StageDistributionWriter(const StageDistributionWriter&) = delete;
  StageDistributionWriter& operator=(const StageDistributionWriter&) = delete;

  void append(const StageDistributionRecord& record);

 private:
  std::filesystem::path output_path_;
  bool enabled_ = false;
  std::ofstream output_;
};

}  // namespace axio::metrics
