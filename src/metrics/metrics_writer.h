/**
 * @file metrics_writer.h
 * @brief Single-owner JSONL writer for synchronized Axio metrics records.
 */
#pragma once

#include "metrics/metrics_record.h"

#include <filesystem>
#include <fstream>

namespace axio::metrics {

class MetricsWriter {
 public:
  MetricsWriter(std::filesystem::path output_path, bool enabled);

  MetricsWriter(const MetricsWriter&) = delete;
  MetricsWriter& operator=(const MetricsWriter&) = delete;

  bool enabled() const { return this->enabled_; }
  const std::filesystem::path& output_path() const {
    return this->output_path_;
  }
  void append(const MetricsRecord& record);

 private:
  std::filesystem::path output_path_;
  bool enabled_ = false;
  std::ofstream output_;
};

}  // namespace axio::metrics
