/**
 * @file metrics_writer.cc
 * @brief Stable JSON serialization and lifecycle for Axio metrics JSONL.
 */
#include "metrics/metrics_writer.h"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace axio::metrics {
namespace {

void write_finite_number(std::ostringstream* output, double value,
                         std::string_view field) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(field) + " must be finite");
  }
  *output << std::fixed << std::setprecision(3) << value;
}

void write_metric(std::ostringstream* output, const MetricValue& metric,
                  std::string_view field) {
  if (!metric.available) {
    *output << "null";
    return;
  }
  write_finite_number(output, metric.value, field);
}

void write_stage(std::ostringstream* output, const StageMetricsRecord& stage,
                 std::string_view name) {
  *output << "{\"completion_time_per_packet_us\":";
  write_metric(output, stage.completion_time_per_packet_us,
               std::string(name) + ".completion_time_per_packet_us");
  *output << ",\"stall_time_per_packet_us\":";
  write_metric(output, stage.stall_time_per_packet_us,
               std::string(name) + ".stall_time_per_packet_us");
  *output << '}';
}

std::string serialize_record(const MetricsRecord& record) {
  if (record.schema != kMetricsSchema) {
    throw std::invalid_argument("unsupported Axio metrics schema: " +
                                record.schema);
  }
  std::ostringstream output;
  output << "{\"window_id\":" << record.window_id
         << ",\"throughput\":{\"e2e_mpps\":";
  write_metric(&output, record.e2e_throughput_mpps,
               "throughput.e2e_mpps");
  output << "},\"latency\":{\"p50_us\":";
  write_metric(&output, record.latency_p50_us, "latency.p50_us");
  output << ",\"p99_us\":";
  write_metric(&output, record.latency_p99_us, "latency.p99_us");
  output << ",\"p999_us\":";
  write_metric(&output, record.latency_p999_us, "latency.p999_us");
  output << "},\"stages\":{\"app_tx\":";
  write_stage(&output, record.app_tx, "app_tx");
  output << ",\"app_rx\":";
  write_stage(&output, record.app_rx, "app_rx");
  output << ",\"dispatcher_tx\":";
  write_stage(&output, record.dispatcher_tx, "dispatcher_tx");
  output << ",\"dispatcher_rx\":";
  write_stage(&output, record.dispatcher_rx, "dispatcher_rx");
  output << ",\"nic_tx\":{\"throughput_mpps\":";
  write_metric(&output, record.nic_tx_throughput_mpps,
               "nic_tx.throughput_mpps");
  output << ",\"submit_time_per_packet_us\":";
  write_metric(&output, record.nic_tx_submit_time_per_packet_us,
               "nic_tx.submit_time_per_packet_us");
  output << "},\"nic_rx\":{\"throughput_mpps\":";
  write_metric(&output, record.nic_rx_throughput_mpps,
               "nic_rx.throughput_mpps");
  output << ",\"completion_interval_cycles\":";
  write_metric(&output, record.nic_rx_completion_interval_cycles,
               "nic_rx.completion_interval_cycles");
  output << ",\"completion_interval_ns\":";
  write_metric(&output, record.nic_rx_completion_interval_ns,
               "nic_rx.completion_interval_ns");
  output << ",\"slowest_interval_cycles\":";
  write_metric(&output, record.nic_rx_slowest_interval_cycles,
               "nic_rx.slowest_interval_cycles");
  output << ",\"capacity_interval_cycles\":";
  write_metric(&output, record.nic_rx_capacity_interval_cycles,
               "nic_rx.capacity_interval_cycles");
  output << "}},\"counters\":{\"app_enqueue_drop_count\":"
         << record.app_enqueue_drop_count
         << ",\"dispatcher_enqueue_drop_count\":"
         << record.dispatcher_enqueue_drop_count
         << ",\"nic_rx_completion_error_count\":"
         << record.nic_rx_completion_error_count << "}}";
  return output.str();
}

}  // namespace

MetricsWriter::MetricsWriter(std::filesystem::path output_path, bool enabled)
    : output_path_(std::move(output_path)), enabled_(enabled) {
  if (!this->enabled_) return;
  const std::filesystem::path parent = this->output_path_.parent_path();
  if (!parent.empty()) {
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) {
      throw std::runtime_error("failed to create metrics directory " +
                               parent.string() + ": " + error.message());
    }
  }
  this->output_.open(this->output_path_, std::ios::out | std::ios::trunc);
  if (!this->output_.is_open()) {
    throw std::runtime_error("failed to open metrics output " +
                             this->output_path_.string());
  }
}

void MetricsWriter::append(const MetricsRecord& record) {
  if (!this->enabled_) return;
  const std::string line = serialize_record(record);
  this->output_ << line << '\n';
  this->output_.flush();
  if (!this->output_) {
    throw std::runtime_error("failed to write metrics output " +
                             this->output_path_.string());
  }
}

void render_human_metrics(const MetricsRecord& record, std::ostream* output) {
  const std::ios::fmtflags original_flags = output->flags();
  const std::streamsize original_precision = output->precision();
  *output << std::fixed << std::setprecision(3);
  const auto metric = [&](const MetricValue& value) {
    if (value.available) {
      *output << value.value;
    } else {
      *output << "N/A";
    }
  };

  *output << "Axio Metrics Window " << record.window_id << '\n';
  *output << "End-to-end throughput (Mpps): ";
  metric(record.e2e_throughput_mpps);
  *output << '\n';
  const auto stage = [&](std::string_view name,
                         const StageMetricsRecord& value) {
    *output << name << " throughput (Mpps): ";
    metric(value.throughput_mpps);
    *output << ", completion (/packet us): ";
    metric(value.completion_time_per_packet_us);
    *output << ", stall (/packet us): ";
    metric(value.stall_time_per_packet_us);
    *output << '\n';
  };
  stage("app_tx", record.app_tx);
  stage("app_rx", record.app_rx);
  stage("dispatcher_tx", record.dispatcher_tx);
  stage("dispatcher_rx", record.dispatcher_rx);
  *output << "NIC TX throughput (Mpps): ";
  metric(record.nic_tx_throughput_mpps);
  *output << ", NIC TX submit (/packet us): ";
  metric(record.nic_tx_submit_time_per_packet_us);
  *output << '\n';
  *output << "NIC RX throughput (Mpps): ";
  metric(record.nic_rx_throughput_mpps);
  *output << ", NIC RX completion interval (ns): ";
  metric(record.nic_rx_completion_interval_ns);
  *output << '\n';
  *output << "Latency p50/p99/p99.9 (us): ";
  metric(record.latency_p50_us);
  *output << '/';
  metric(record.latency_p99_us);
  *output << '/';
  metric(record.latency_p999_us);
  *output << "\n\n";
  output->flags(original_flags);
  output->precision(original_precision);
}

MetricsPublisher::MetricsPublisher(MetricsWriter* writer, bool human_output,
                                   std::ostream* output)
    : writer_(writer), human_output_(human_output), output_(output) {
  if (this->writer_ == nullptr) {
    throw std::invalid_argument("metrics publisher requires a writer");
  }
  if (this->human_output_ && this->output_ == nullptr) {
    throw std::invalid_argument(
        "human metrics output requires an output stream");
  }
}

void MetricsPublisher::publish(MetricsRecord record) {
  record.window_id = this->next_window_id_;
  this->writer_->append(record);
  if (this->human_output_) {
    render_human_metrics(record, this->output_);
    this->output_->flush();
    if (!*this->output_) {
      throw std::runtime_error("failed to write human metrics output");
    }
  }
  this->next_window_id_++;
}

}  // namespace axio::metrics
