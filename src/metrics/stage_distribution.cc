/**
 * @file stage_distribution.cc
 * @brief Stage-distribution sampling and stable JSONL serialization.
 */
#include "metrics/stage_distribution.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace axio::metrics {
namespace {

size_t percentile_index(size_t sample_count, uint32_t percentile) {
  const double rank = std::ceil(
      static_cast<double>(percentile) * sample_count / 100.0);
  return static_cast<size_t>(std::max(1.0, rank)) - 1;
}

void write_number(std::ostringstream* output, double value) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument("stage-distribution value must be finite");
  }
  *output << std::fixed << std::setprecision(2) << value;
}

void write_summary(std::ostringstream* output,
                   const DistributionSummary& summary) {
  const auto metric = [&](double value) {
    if (summary.available) {
      write_number(output, value);
    } else {
      *output << "null";
    }
  };
  *output << "{\"unit\":\"us_per_batch\",\"p1_us\":";
  metric(summary.p1);
  *output << ",\"p50_us\":";
  metric(summary.p50);
  *output << ",\"p99_us\":";
  metric(summary.p99);
  *output << ",\"mean_us\":";
  metric(summary.mean);
  *output << ",\"min_us\":";
  metric(summary.minimum);
  *output << ",\"max_us\":";
  metric(summary.maximum);
  *output << ",\"sample_count\":" << summary.sample_count << '}';
}

}  // namespace

StageDistributionSampler::StageDistributionSampler(size_t sample_stride,
                                                   size_t sample_capacity)
    : sample_stride_(sample_stride), sample_capacity_(sample_capacity) {
  if (sample_stride == 0 || sample_capacity == 0) {
    throw std::invalid_argument(
        "stage-distribution stride and capacity must be positive");
  }
  this->samples_.reserve(sample_capacity);
}

void StageDistributionSampler::record(uint64_t cycles) {
  ++this->observation_count_;
  if (this->observation_count_ % this->sample_stride_ != 0) return;
  if (this->samples_.size() < this->sample_capacity_) {
    this->samples_.push_back(cycles);
    return;
  }
  this->samples_[this->next_index_] = cycles;
  this->next_index_ = (this->next_index_ + 1) % this->sample_capacity_;
  this->wrapped_ = true;
}

std::vector<uint64_t> StageDistributionSampler::samples() const {
  if (!this->wrapped_) return this->samples_;
  std::vector<uint64_t> ordered;
  ordered.reserve(this->samples_.size());
  ordered.insert(ordered.end(), this->samples_.begin() + this->next_index_,
                 this->samples_.end());
  ordered.insert(ordered.end(), this->samples_.begin(),
                 this->samples_.begin() + this->next_index_);
  return ordered;
}

std::vector<uint64_t> StageDistributionSampler::take_samples() {
  std::vector<uint64_t> result = this->samples();
  this->samples_.clear();
  this->observation_count_ = 0;
  this->next_index_ = 0;
  this->wrapped_ = false;
  return result;
}

DistributionSummary summarize_distribution(std::vector<double> samples) {
  DistributionSummary summary;
  summary.sample_count = samples.size();
  if (samples.empty()) return summary;
  for (const double value : samples) {
    if (!std::isfinite(value)) {
      throw std::invalid_argument(
          "stage-distribution samples must be finite");
    }
  }
  std::sort(samples.begin(), samples.end());
  summary.available = true;
  summary.p1 = samples[percentile_index(samples.size(), 1)];
  summary.p50 = samples[percentile_index(samples.size(), 50)];
  summary.p99 = samples[percentile_index(samples.size(), 99)];
  summary.minimum = samples.front();
  summary.maximum = samples.back();
  summary.mean = std::accumulate(samples.begin(), samples.end(), 0.0) /
                 static_cast<double>(samples.size());
  return summary;
}

StageDistributionWriter::StageDistributionWriter(
    std::filesystem::path output_path, bool enabled)
    : output_path_(std::move(output_path)), enabled_(enabled) {
  if (!enabled) return;
  const std::filesystem::path parent = this->output_path_.parent_path();
  if (!parent.empty()) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
      throw std::runtime_error("failed to create stage-distribution directory " +
                               parent.string() + ": " + error.message());
    }
  }
  this->output_.open(this->output_path_, std::ios::out | std::ios::trunc);
  if (!this->output_.is_open()) {
    throw std::runtime_error("failed to open stage-distribution output " +
                             this->output_path_.string());
  }
}

void StageDistributionWriter::append(const StageDistributionRecord& record) {
  if (!this->enabled_) return;
  std::ostringstream output;
  output << "{\"schema\":\"" << kStageDistributionSchema
         << "\",\"window_id\":" << record.window_id
         << ",\"sample_stride\":" << record.sample_stride
         << ",\"stages\":{\"app_tx_allocation_stall\":";
  write_summary(&output, record.app_tx_allocation_stall);
  output << ",\"app_rx_handler_completion\":";
  write_summary(&output, record.app_rx_handler_completion);
  output << "}}";
  this->output_ << output.str() << '\n';
  this->output_.flush();
  if (!this->output_) {
    throw std::runtime_error("failed to write stage-distribution output " +
                             this->output_path_.string());
  }
}

}  // namespace axio::metrics
