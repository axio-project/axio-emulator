/**
 * @file rx_completion_window.h
 * @brief Queue-local host-visible RX completion interval accounting.
 */
#pragma once

#include <cstdint>
#include <optional>

namespace axio::metrics {

struct CompletionTimestamp {
  uint64_t cycles = 0;
  uint32_t cpu_id = 0;
};

struct RxCompletionSnapshot {
  uint64_t total_completion_count = 0;
  uint64_t timed_completion_count = 0;
  uint64_t successful_poll_count = 0;
  uint64_t empty_poll_count = 0;
  uint64_t completion_error_count = 0;
  uint64_t window_generation = 0;
  uint64_t first_completion_tsc = 0;
  uint64_t last_completion_tsc = 0;
  bool clock_valid = true;
  std::optional<double> mean_interval_cycles;
};

class RxCompletionWindow {
 public:
  void observe(CompletionTimestamp timestamp, uint32_t successful_count) {
    if (successful_count == 0) {
      this->observe_empty_poll();
      return;
    }
    this->total_completion_count_ += successful_count;
    this->successful_poll_count_++;
    if (!this->has_anchor_) {
      this->first_completion_tsc_ = timestamp.cycles;
      this->last_completion_tsc_ = timestamp.cycles;
      this->anchor_cpu_id_ = timestamp.cpu_id;
      this->has_anchor_ = true;
      return;
    }
    if (timestamp.cpu_id != this->anchor_cpu_id_) {
      this->clock_valid_ = false;
    }
    if (timestamp.cycles <= this->last_completion_tsc_) {
      this->clock_valid_ = false;
    }
    this->timed_completion_count_ += successful_count;
    this->last_completion_tsc_ = timestamp.cycles;
  }

  void observe_empty_poll() { this->empty_poll_count_++; }

  void observe_completion_errors(uint32_t error_count) {
    this->completion_error_count_ += error_count;
  }

  void reset() {
    const uint64_t next_generation = this->window_generation_ + 1;
    *this = RxCompletionWindow{};
    this->window_generation_ = next_generation;
  }

  RxCompletionSnapshot snapshot() const {
    RxCompletionSnapshot result;
    result.total_completion_count = this->total_completion_count_;
    result.timed_completion_count = this->timed_completion_count_;
    result.successful_poll_count = this->successful_poll_count_;
    result.empty_poll_count = this->empty_poll_count_;
    result.completion_error_count = this->completion_error_count_;
    result.window_generation = this->window_generation_;
    result.first_completion_tsc = this->first_completion_tsc_;
    result.last_completion_tsc = this->last_completion_tsc_;
    result.clock_valid = this->clock_valid_;
    if (this->successful_poll_count_ >= 2 &&
        this->timed_completion_count_ != 0 && this->clock_valid_ &&
        this->completion_error_count_ == 0) {
      result.mean_interval_cycles =
          static_cast<double>(this->last_completion_tsc_ -
                              this->first_completion_tsc_) /
          static_cast<double>(this->timed_completion_count_);
    }
    return result;
  }

 private:
  uint64_t total_completion_count_ = 0;
  uint64_t timed_completion_count_ = 0;
  uint64_t successful_poll_count_ = 0;
  uint64_t empty_poll_count_ = 0;
  uint64_t completion_error_count_ = 0;
  uint64_t window_generation_ = 0;
  uint64_t first_completion_tsc_ = 0;
  uint64_t last_completion_tsc_ = 0;
  uint32_t anchor_cpu_id_ = 0;
  bool has_anchor_ = false;
  bool clock_valid_ = true;
};

}  // namespace axio::metrics
