/**
 * @file metrics_writer.h
 * @brief Single-owner JSONL writer for synchronized Axio metrics records.
 */
#pragma once

#include "metrics/metrics_record.h"

#include <filesystem>
#include <fstream>
#include <ostream>

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

void render_human_metrics(const MetricsRecord& record, std::ostream* output);

class MetricsPublisher {
 public:
  MetricsPublisher(MetricsWriter* writer, bool human_output,
                   std::ostream* output);

  void publish(MetricsRecord record);
  uint64_t next_window_id() const { return this->next_window_id_; }

 private:
  MetricsWriter* writer_ = nullptr;
  bool human_output_ = false;
  std::ostream* output_ = nullptr;
  uint64_t next_window_id_ = 0;
};

}  // namespace axio::metrics
