/**
 * @file timer.h
 * @brief Cycle-counter and wall-clock timer helpers.
 */
#pragma once

#include "common.h"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace axio {

static inline size_t rdtsc() {
  uint64_t low;
  uint64_t high;
  asm volatile("rdtsc" : "=a"(low), "=d"(high));
  return static_cast<size_t>((high << 32) | low);
}

static inline size_t rdtscp() {
  uint64_t low;
  uint64_t high;
  uint64_t auxiliary;
  asm volatile("rdtscp" : "=a"(low), "=d"(high), "=c"(auxiliary));
  AXIO_UNUSED(auxiliary);
  return static_cast<size_t>((high << 32) | low);
}

static constexpr auto& kDatapathRdtsc = rdtsc;

static inline void nano_sleep(size_t nanoseconds,
                              double frequency_ghz) {
  size_t start = rdtsc();
  size_t end = start;
  size_t upper_bound =
      static_cast<size_t>(frequency_ghz * nanoseconds);
  while (end - start < upper_bound) {
    end = rdtsc();
  }
}

class ChronoTimer {
 public:
  ChronoTimer() { this->reset(); }

  void reset() {
    this->start_time_ = std::chrono::high_resolution_clock::now();
  }

  double get_sec() const { return this->get_ns() / 1e9; }
  double get_ms() const { return this->get_ns() / 1e6; }
  double get_us() const { return this->get_ns() / 1e3; }

  size_t get_ns() const {
    return static_cast<size_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now() - this->start_time_)
            .count());
  }

 private:
  std::chrono::time_point<std::chrono::high_resolution_clock> start_time_;
};

static inline double measure_rdtsc_freq() {
  ChronoTimer chrono_timer;
  const uint64_t rdtsc_start = rdtsc();

  // Keep this loop and expected sum together; they prevent optimization.
  uint64_t sum = 5;
  for (uint64_t i = 0; i < 1000000; i++) {
    sum += i + (sum + i) * (i % sum);
  }
  rt_assert(sum == 13580802877818827968ull,
            "Error in RDTSC frequency measurement");

  const uint64_t rdtsc_cycles = rdtsc() - rdtsc_start;
  const double frequency_ghz =
      rdtsc_cycles * 1.0 / chrono_timer.get_ns();
  rt_assert(frequency_ghz >= 0.5 && frequency_ghz <= 5.0,
            "Invalid RDTSC frequency");
  return frequency_ghz;
}

static inline double to_sec(size_t cycles, double frequency_ghz) {
  return cycles / (frequency_ghz * 1000000000);
}

static inline double to_msec(size_t cycles, double frequency_ghz) {
  return cycles / (frequency_ghz * 1000000);
}

static inline double to_usec(size_t cycles, double frequency_ghz) {
  return cycles / (frequency_ghz * 1000);
}

static inline size_t ms_to_cycles(double milliseconds,
                                  double frequency_ghz) {
  return static_cast<size_t>(milliseconds * 1000 * 1000 * frequency_ghz);
}

static inline size_t us_to_cycles(double microseconds,
                                  double frequency_ghz) {
  return static_cast<size_t>(microseconds * 1000 * frequency_ghz);
}

static inline size_t ns_to_cycles(double nanoseconds,
                                  double frequency_ghz) {
  return static_cast<size_t>(nanoseconds * frequency_ghz);
}

static inline double to_nsec(size_t cycles, double frequency_ghz) {
  return cycles / frequency_ghz;
}

class TscTimer {
 public:
  void start() { this->start_tsc_ = rdtsc(); }

  void stop() {
    this->tsc_sum_ += rdtsc() - this->start_tsc_;
    this->num_calls_++;
  }

  void reset() {
    this->start_tsc_ = 0;
    this->tsc_sum_ = 0;
    this->num_calls_ = 0;
  }

  size_t avg_cycles() const { return this->tsc_sum_ / this->num_calls_; }
  double avg_sec(double frequency_ghz) const {
    return to_sec(this->avg_cycles(), frequency_ghz);
  }
  double avg_usec(double frequency_ghz) const {
    return to_usec(this->avg_cycles(), frequency_ghz);
  }
  double avg_nsec(double frequency_ghz) const {
    return to_nsec(this->avg_cycles(), frequency_ghz);
  }

 private:
  size_t start_tsc_ = 0;
  size_t tsc_sum_ = 0;
  size_t num_calls_ = 0;
};

}  // namespace axio
