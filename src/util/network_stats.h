/*
 * Copyright (c) 2021-2022 Baidu.com, Inc. All Rights Reserved.
 * Copyright (c) 2022-2023 Jianzhang Peng. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Jianzhang Peng (pengjianzhang@baidu.com)
 *         Jianzhang Peng (pengjianzhang@gmail.com)
 */

#pragma once

#include <iomanip>
#include <iostream>
#include <limits>

#include "common.h"
#include "metrics/rx_completion_window.h"

namespace axio {

struct NetworkStats {
  /* App level */
  uint64_t app_tx_message_count_ = 0;
  uint64_t app_rx_message_count_ = 0;

  uint64_t app_tx_invocation_count_ = 0;
  uint64_t app_tx_total_duration_ = 0;
  uint64_t app_tx_max_duration_ = 0;
  uint64_t app_tx_min_duration_ = std::numeric_limits<uint64_t>::max();
  uint64_t app_tx_stall_total_duration_ = 0;
  uint64_t app_tx_stall_max_duration_ = 0;
  uint64_t app_tx_stall_min_duration_ = std::numeric_limits<uint64_t>::max();

  void* app_tx_mbuf_trace_address_ = nullptr;
  uint64_t app_tx_mbuf_reuse_temporary_interval_ = 0;
  uint64_t app_tx_mbuf_reuse_interval_ = 0;
  uint64_t app_tx_traced_mbuf_count_ = 0;

  uint64_t app_rx_invocation_count_ = 0;
  uint64_t app_rx_total_duration_ = 0;
  uint64_t app_rx_max_duration_ = 0;
  uint64_t app_rx_min_duration_ = std::numeric_limits<uint64_t>::max();
  uint64_t app_rx_stall_total_duration_ = 0;
  uint64_t app_rx_stall_max_duration_ = 0;
  uint64_t app_rx_stall_min_duration_ = 0;

  /* Dispatcher level */
  uint64_t dispatcher_tx_packet_count_ = 0;
  uint64_t dispatcher_rx_packet_count_ = 0;
  uint64_t dispatcher_tx_duration_ = 0;
  uint64_t dispatcher_tx_stall_duration_ = 0;
  uint64_t dispatcher_rx_duration_ = 0;
  uint64_t dispatcher_rx_stall_duration_ = 0;

  /* NIC level */
  uint64_t nic_tx_packet_count_ = 0;
  uint64_t nic_rx_packet_count_ = 0;
  uint64_t nic_tx_duration_ = 0;
  metrics::RxCompletionWindow nic_rx_completion_window_;

  /* Diagnose */
  uint64_t app_mbuf_stall_count_ = 0;
  uint64_t app_enqueue_drop_count_ = 0;
  uint32_t mbuf_allocation_count_ = 0;
  uint64_t mbuf_usage_total_ = 0;
  uint64_t dispatcher_enqueue_drop_count_ = 0;
};

struct PerformanceStats {
  double e2e_throughput_ = 0;  // Mpps
  double e2e_compl_ = 0;       // us

  double app_tx_throughput_ = 0;
  double app_rx_throughput_ = 0;

  double app_tx_compl_ = 0;
  double app_tx_compl_max_ = 0;
  double app_tx_compl_min_ = std::numeric_limits<uint64_t>::max();
  double app_tx_compl_avg_ = 0;
  double app_tx_stall_ = 0;
  double app_tx_stall_avg_ = 0;
  double app_tx_stall_min_ = 0;
  double app_tx_stall_max_ = 0;

  double app_rx_compl_ = 0;
  double app_rx_compl_max_ = 0;
  double app_rx_compl_min_ = std::numeric_limits<uint64_t>::max();
  double app_rx_compl_avg_ = 0;
  double app_rx_stall_ = 0;
  double app_rx_stall_avg_ = 0;
  double app_rx_stall_min_ = 0;
  double app_rx_stall_max_ = 0;

  double disp_tx_throughput_ = 0;
  double disp_rx_throughput_ = 0;
  double disp_tx_compl_ = 0;
  double disp_tx_stall_ = 0;
  double disp_rx_compl_ = 0;
  double disp_rx_stall_ = 0;

  double dispatcher_mbuf_usage_ = 0;

  double nic_tx_throughput_ = 0;
  double nic_rx_throughput_ = 0;
  double nic_tx_compl_ = 0;
  double nic_rx_compl_ = 0;
  double nic_rx_slowest_compl_ = 0;
  double nic_rx_capacity_compl_ = 0;
  uint64_t nic_rx_timed_completion_count_ = 0;
  bool nic_rx_completion_valid_ = false;

  void print() {
    this->app_tx_stall_min_ =
        this->app_tx_stall_min_ > this->app_tx_stall_max_
            ? 9999 : this->app_tx_stall_min_;
    this->app_rx_stall_min_ =
        this->app_rx_stall_min_ > this->app_rx_stall_max_
            ? 9999 : this->app_rx_stall_min_;
    this->app_tx_compl_min_ =
        this->app_tx_compl_min_ > this->app_tx_compl_max_
            ? 9999 : this->app_tx_compl_min_;
    this->app_rx_compl_min_ =
        this->app_rx_compl_min_ > this->app_rx_compl_max_
            ? 9999 : this->app_rx_compl_min_;

#if AXIO_NODE_TYPE == AXIO_SERVER
    this->e2e_throughput_ = this->disp_tx_throughput_;
    this->e2e_compl_ = 1.0 / this->e2e_throughput_;
#elif AXIO_NODE_TYPE == AXIO_CLIENT
    this->e2e_throughput_ = this->disp_rx_throughput_;
    this->e2e_compl_ = 1.0 / this->e2e_throughput_;
#endif

    constexpr const char* kSeparator =
        "---------------------------------------------------------------------";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << kSeparator << kSeparator << kSeparator << std::endl;
    std::cout << std::left << std::setw(20) << "Perf Statistics"
              << std::setw(20) << "Thpl. (Mpps)"
              << std::setw(20) << "Avg. [/P]"
              << std::setw(20) << "Avg. Stall [/P]"
              << std::setw(20) << "Max Stall. [/B]"
              << std::setw(20) << "Min Stall. [/B]"
              << std::setw(20) << "Avg Stall. [/B]"
              << std::setw(20) << "Max Coml. [/B]"
              << std::setw(20) << "Min Coml. [/B]"
              << std::setw(20) << "Avg Coml. [/B]" << std::endl;
    std::cout << kSeparator << kSeparator << kSeparator << std::endl;

    std::cout << std::left << std::setw(20) << "End-to-end"
              << std::setw(20) << this->e2e_throughput_ << std::setw(20)
              << this->e2e_compl_ << std::endl;
    std::cout << std::left << std::setw(20) << "app_tx"
              << std::setw(20) << this->app_tx_throughput_ << std::setw(20)
              << this->app_tx_compl_ + this->app_tx_stall_ << std::setw(20)
              << this->app_tx_stall_
              << std::setw(20) << this->app_tx_stall_max_ << std::setw(20)
              << this->app_tx_stall_min_ << std::setw(20)
              << std::to_string(this->app_tx_stall_avg_) + "(" +
                     std::to_string(this->dispatcher_mbuf_usage_) + ")"
              << std::setw(20) << this->app_tx_compl_max_ << std::setw(20)
              << this->app_tx_compl_min_ << std::setw(20)
              << this->app_tx_compl_avg_
              << std::endl;
    std::cout << std::left << std::setw(20) << "app_rx"
              << std::setw(20) << this->app_rx_throughput_ << std::setw(20)
              << this->app_rx_compl_ + this->app_rx_stall_ << std::setw(20)
              << this->app_rx_stall_
              << std::setw(20) << this->app_rx_stall_max_ << std::setw(20)
              << this->app_rx_stall_min_ << std::setw(20)
              << this->app_rx_stall_avg_
              << std::setw(20) << this->app_rx_compl_max_ << std::setw(20)
              << this->app_rx_compl_min_ << std::setw(20)
              << this->app_rx_compl_avg_
              << std::endl;
    std::cout << std::left << std::setw(20) << "disp_tx"
              << std::setw(20) << this->disp_tx_throughput_ << std::setw(20)
              << this->disp_tx_compl_ + this->disp_tx_stall_ << std::setw(20)
              << this->disp_tx_stall_ << std::endl;
    std::cout << std::left << std::setw(20) << "disp_rx"
              << std::setw(20) << this->disp_rx_throughput_ << std::setw(20)
              << this->disp_rx_compl_ + this->disp_rx_stall_ << std::setw(20)
              << this->disp_rx_stall_ << std::endl;
    std::cout << std::left << std::setw(20) << "nic_tx"
              << std::setw(20) << this->nic_tx_throughput_ << std::setw(15)
              << this->nic_tx_compl_ << std::endl;
    std::cout << std::left << std::setw(20) << "nic_rx"
              << std::setw(20) << this->nic_rx_throughput_ << std::setw(15)
              << this->nic_rx_compl_ << std::endl;
    std::cout << kSeparator << kSeparator << kSeparator << std::endl;
    std::cout << std::endl;
  }
};

#define AXIO_RECORD_APP_TX(n) \
  do { this->stats_->app_tx_message_count_ += (n); } while (0)
#define AXIO_RECORD_APP_RX(n) \
  do { this->stats_->app_rx_message_count_ += (n); } while (0)

#if AXIO_PERF_TEST_LATENCY == 1 && AXIO_PERF_TEST_LATENCY_MIN_MAX == 1
#define AXIO_RECORD_APP_TX_DURATION(n) do {                                      \
  uint64_t duration_tick = rdtsc() - (n);                                        \
  this->stats_->app_tx_invocation_count_ += 1;                                   \
  this->stats_->app_tx_total_duration_ += duration_tick;                         \
  this->stats_->app_tx_min_duration_ =                                          \
      this->stats_->app_tx_min_duration_ > duration_tick                        \
          ? duration_tick : this->stats_->app_tx_min_duration_;                 \
  this->stats_->app_tx_max_duration_ =                                          \
      this->stats_->app_tx_max_duration_ < duration_tick                        \
          ? duration_tick : this->stats_->app_tx_max_duration_;                 \
} while (0)
#define AXIO_RECORD_APP_TX_STALL_DURATION(n) do {                                \
  uint64_t duration_tick = rdtsc() - (n);                                        \
  this->stats_->app_tx_stall_total_duration_ += duration_tick;                   \
  this->stats_->app_tx_stall_max_duration_ =                                    \
      this->stats_->app_tx_stall_max_duration_ < duration_tick                  \
          ? duration_tick : this->stats_->app_tx_stall_max_duration_;           \
  this->stats_->app_tx_stall_min_duration_ =                                    \
      this->stats_->app_tx_stall_min_duration_ > duration_tick                  \
          ? duration_tick : this->stats_->app_tx_stall_min_duration_;           \
} while (0)

#define AXIO_RECORD_APP_RX_DURATION(n) do {                                      \
  uint64_t duration_tick = rdtsc() - (n);                                        \
  this->stats_->app_rx_invocation_count_ += 1;                                   \
  this->stats_->app_rx_total_duration_ += duration_tick;                         \
  this->stats_->app_rx_min_duration_ =                                          \
      this->stats_->app_rx_min_duration_ > duration_tick                        \
          ? duration_tick : this->stats_->app_rx_min_duration_;                 \
  this->stats_->app_rx_max_duration_ =                                          \
      this->stats_->app_rx_max_duration_ < duration_tick                        \
          ? duration_tick : this->stats_->app_rx_max_duration_;                 \
} while (0)
#define AXIO_RECORD_APP_RX_STALL_DURATION(n) do {                                \
  uint64_t duration_tick = rdtsc() - (n);                                        \
  this->stats_->app_rx_stall_total_duration_ += duration_tick;                   \
  this->stats_->app_rx_stall_max_duration_ =                                    \
      this->stats_->app_rx_stall_max_duration_ < duration_tick                  \
          ? duration_tick : this->stats_->app_rx_stall_max_duration_;           \
  this->stats_->app_rx_stall_min_duration_ =                                    \
      this->stats_->app_rx_stall_min_duration_ > duration_tick                  \
          ? duration_tick : this->stats_->app_rx_stall_min_duration_;           \
} while (0)
#elif AXIO_PERF_TEST_LATENCY == 1 && AXIO_PERF_TEST_LATENCY_MIN_MAX == 0
#define AXIO_RECORD_APP_TX_DURATION(n) do {                                    \
    this->stats_->app_tx_total_duration_ += rdtsc() - (n);                     \
} while (0)
#define AXIO_RECORD_APP_TX_STALL_DURATION(n) do {                              \
    this->stats_->app_tx_stall_total_duration_ += rdtsc() - (n);               \
} while (0)
#define AXIO_RECORD_APP_RX_DURATION(n) do {                                    \
    this->stats_->app_rx_total_duration_ += rdtsc() - (n);                     \
} while (0)
#define AXIO_RECORD_APP_RX_STALL_DURATION(n) do {                              \
    this->stats_->app_rx_stall_total_duration_ += rdtsc() - (n);               \
} while (0)
#endif

#if AXIO_PERF_TEST_MBUF_RANGE == 1
#define AXIO_RECORD_APP_TX_MBUF_REUSE_INTERVAL(mbuf_address) do {              \
  if (AXIO_UNLIKELY(this->stats_->app_tx_mbuf_trace_address_ == nullptr)) {     \
    this->stats_->app_tx_mbuf_trace_address_ = (mbuf_address);                  \
    this->stats_->app_tx_traced_mbuf_count_ += 1;                               \
  } else if (AXIO_UNLIKELY(                                                    \
                 (mbuf_address) == this->stats_->app_tx_mbuf_trace_address_)) { \
    this->stats_->app_tx_mbuf_reuse_interval_ +=                               \
        this->stats_->app_tx_mbuf_reuse_temporary_interval_;                   \
    this->stats_->app_tx_mbuf_trace_address_ = nullptr;                         \
    this->stats_->app_tx_mbuf_reuse_temporary_interval_ = 0;                    \
  } else {                                                                      \
    this->stats_->app_tx_mbuf_reuse_temporary_interval_ += 1;                   \
  }                                                                             \
} while (0)
#else
#define AXIO_RECORD_APP_TX_MBUF_REUSE_INTERVAL(mbuf_address) \
  do { } while (0)
#endif

#define AXIO_RECORD_DISPATCHER_TX(n) \
  do { this->stats_->dispatcher_tx_packet_count_ += (n); } while (0)
#define AXIO_RECORD_DISPATCHER_RX(n) \
  do { this->stats_->dispatcher_rx_packet_count_ += (n); } while (0)
#define AXIO_RECORD_DISPATCHER_TX_DURATION(n) \
  do { this->stats_->dispatcher_tx_duration_ += rdtsc() - (n); } while (0)
#define AXIO_RECORD_DISPATCHER_TX_STALL_DURATION(n) \
  do { this->stats_->dispatcher_tx_stall_duration_ += rdtsc() - (n); } while (0)
#define AXIO_RECORD_DISPATCHER_RX_DURATION(n) \
  do { this->stats_->dispatcher_rx_duration_ += rdtsc() - (n); } while (0)
#define AXIO_RECORD_DISPATCHER_RX_STALL_DURATION(n) \
  do { this->stats_->dispatcher_rx_stall_duration_ += rdtsc() - (n); } while (0)

#define AXIO_RECORD_NIC_TX(n) \
  do { this->stats_->nic_tx_packet_count_ += (n); } while (0)
#define AXIO_RECORD_NIC_RX(n) \
  do { this->stats_->nic_rx_packet_count_ += (n); } while (0)
#define AXIO_RECORD_NIC_TX_DURATION(start_tick) \
  do { this->stats_->nic_tx_duration_ += rdtsc() - (start_tick); } while (0)

/* Diagnose */
#define AXIO_RECORD_APP_MBUF_STALL() \
  do { this->stats_->app_mbuf_stall_count_++; } while (0)
#define AXIO_RECORD_APP_DROP(n) \
  do { this->stats_->app_enqueue_drop_count_ += (n); } while (0)
#define AXIO_RECORD_MBUF_USAGE(n) do {       \
  this->stats_->mbuf_allocation_count_++;    \
  this->stats_->mbuf_usage_total_ += (n);    \
} while (0)
#define AXIO_RECORD_DISPATCHER_DROP(n) \
  do { this->stats_->dispatcher_enqueue_drop_count_ += (n); } while (0)

inline void initialize_network_stats(NetworkStats* stats) {
  metrics::RxCompletionWindow completion_window =
      stats->nic_rx_completion_window_;
  completion_window.reset();
  *stats = {};
  stats->nic_rx_completion_window_ = completion_window;
  stats->app_tx_min_duration_ = std::numeric_limits<uint64_t>::max();
  stats->app_rx_min_duration_ = std::numeric_limits<uint64_t>::max();
  stats->app_tx_stall_min_duration_ = std::numeric_limits<uint64_t>::max();
  stats->app_rx_stall_min_duration_ = std::numeric_limits<uint64_t>::max();
}

inline void initialize_performance_stats(PerformanceStats* stats) {
  *stats = {};
  stats->app_tx_compl_min_ = std::numeric_limits<uint64_t>::max();
  stats->app_rx_compl_min_ = std::numeric_limits<uint64_t>::max();
  stats->app_tx_stall_min_ = std::numeric_limits<uint64_t>::max();
  stats->app_rx_stall_min_ = std::numeric_limits<uint64_t>::max();
}

}  // namespace axio
