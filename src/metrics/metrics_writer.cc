/**
 * @file metrics_writer.cc
 * @brief Stable JSON serialization and lifecycle for Axio metrics JSONL.
 */
#include "metrics/metrics_writer.h"

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace axio::metrics {
namespace {

void write_json_string(std::ostringstream* output, std::string_view value) {
  *output << '"';
  for (const unsigned char character : value) {
    switch (character) {
      case '"': *output << "\\\""; break;
      case '\\': *output << "\\\\"; break;
      case '\b': *output << "\\b"; break;
      case '\f': *output << "\\f"; break;
      case '\n': *output << "\\n"; break;
      case '\r': *output << "\\r"; break;
      case '\t': *output << "\\t"; break;
      default:
        if (character < 0x20) {
          *output << "\\u00" << std::hex << std::setw(2)
                  << std::setfill('0') << static_cast<unsigned int>(character)
                  << std::dec << std::setfill(' ');
        } else {
          *output << static_cast<char>(character);
        }
    }
  }
  *output << '"';
}

void write_finite_number(std::ostringstream* output, double value,
                         std::string_view field) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(field) + " must be finite");
  }
  *output << std::setprecision(std::numeric_limits<double>::max_digits10)
          << value;
}

void write_metric(std::ostringstream* output, const MetricValue& metric,
                  std::string_view field) {
  if (!metric.available) {
    *output << "null";
    return;
  }
  write_finite_number(output, metric.value, field);
}

void write_string_array(std::ostringstream* output,
                        const std::vector<std::string>& values) {
  *output << '[';
  for (size_t index = 0; index < values.size(); ++index) {
    if (index != 0) *output << ',';
    write_json_string(output, values[index]);
  }
  *output << ']';
}

void write_uint32_array(std::ostringstream* output,
                        const std::vector<uint32_t>& values) {
  *output << '[';
  for (size_t index = 0; index < values.size(); ++index) {
    if (index != 0) *output << ',';
    *output << values[index];
  }
  *output << ']';
}

void write_stage(std::ostringstream* output, const StageMetricsRecord& stage,
                 std::string_view name) {
  *output << "{\"throughput_mpps\":";
  write_metric(output, stage.throughput_mpps,
               std::string(name) + ".throughput_mpps");
  *output << ",\"completion_time_per_packet_us\":";
  write_metric(output, stage.completion_time_per_packet_us,
               std::string(name) + ".completion_time_per_packet_us");
  *output << ",\"stall_time_per_packet_us\":";
  write_metric(output, stage.stall_time_per_packet_us,
               std::string(name) + ".stall_time_per_packet_us");
  *output << '}';
}

void write_queue(std::ostringstream* output, const QueueMetricsRecord& queue) {
  *output << "{\"workspace_id\":" << queue.workspace_id
          << ",\"workload_ids\":";
  write_uint32_array(output, queue.workload_ids);
  *output << ",\"successful_completion_count\":"
          << queue.successful_completion_count
          << ",\"timed_completion_count\":"
          << queue.timed_completion_count
          << ",\"successful_poll_count\":" << queue.successful_poll_count
          << ",\"empty_poll_count\":" << queue.empty_poll_count
          << ",\"completion_error_count\":"
          << queue.completion_error_count
          << ",\"first_completion_tsc\":" << queue.first_completion_tsc
          << ",\"last_completion_tsc\":" << queue.last_completion_tsc
          << ",\"tsc_frequency_ghz\":";
  write_finite_number(output, queue.tsc_frequency_ghz,
                      "queue.tsc_frequency_ghz");
  *output << ",\"clock_valid\":" << (queue.clock_valid ? "true" : "false")
          << ",\"measurement_valid\":"
          << (queue.measurement_valid ? "true" : "false")
          << ",\"capacity_comparable\":"
          << (queue.capacity_comparable ? "true" : "false")
          << ",\"invalid_reasons\":";
  write_string_array(output, queue.invalid_reasons);
  *output << ",\"completion_interval_cycles\":";
  write_metric(output, queue.completion_interval_cycles,
               "queue.completion_interval_cycles");
  *output << ",\"completion_interval_ns\":";
  write_metric(output, queue.completion_interval_ns,
               "queue.completion_interval_ns");
  *output << ",\"completion_rate_mpps\":";
  write_metric(output, queue.completion_rate_mpps,
               "queue.completion_rate_mpps");
  *output << '}';
}

std::string serialize_record(const MetricsRecord& record) {
  if (record.schema != kMetricsSchema) {
    throw std::invalid_argument("unsupported Axio metrics schema: " +
                                record.schema);
  }
  std::ostringstream output;
  output << "{\"schema\":";
  write_json_string(&output, record.schema);
  output << ",\"run_id\":";
  write_json_string(&output, record.run_id);
  output << ",\"window_id\":" << record.window_id
         << ",\"identity\":{\"role\":";
  write_json_string(&output, record.role);
  output << ",\"backend\":";
  write_json_string(&output, record.backend);
  output << ",\"version\":";
  write_json_string(&output, record.version);
  output << ",\"git_commit\":";
  write_json_string(&output, record.git_commit);
  output << ",\"build_fingerprint\":";
  write_json_string(&output, record.build_fingerprint);
  output << ",\"config_fingerprint\":";
  write_json_string(&output, record.config_fingerprint);
  output << "},\"window\":{\"duration_seconds\":";
  write_finite_number(&output, record.duration_seconds,
                      "window.duration_seconds");
  output << ",\"measurement_valid\":"
         << (record.measurement_valid ? "true" : "false")
         << ",\"capacity_comparable\":"
         << (record.capacity_comparable ? "true" : "false")
         << ",\"invalid_reasons\":";
  write_string_array(&output, record.invalid_reasons);
  output << "},\"throughput\":{\"e2e_mpps\":";
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
         << ",\"nic_tx_packet_count\":" << record.nic_tx_packet_count
         << ",\"nic_rx_successful_completion_count\":"
         << record.nic_rx_successful_completion_count
         << ",\"nic_rx_timed_completion_count\":"
         << record.nic_rx_timed_completion_count
         << ",\"nic_rx_completion_error_count\":"
         << record.nic_rx_completion_error_count << "},\"queues\":[";
  for (size_t index = 0; index < record.queues.size(); ++index) {
    if (index != 0) output << ',';
    write_queue(&output, record.queues[index]);
  }
  output << "]}";
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
