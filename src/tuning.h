/**
 * @file tuning.h
 * @brief Tunable parameters, including cpu cores, queue numbers and batch sizes
 */
#pragma once

#include "common.h"

namespace dperf {
struct tunable_params {
  /**
   * ----------------------Application level----------------------
   */
  uint8_t kAppCPUCores = 1;
  #ifdef OneStage
    size_t kAppBatchSize = FlowSize;
  #else
    size_t kAppBatchSize = 32;
  #endif
  /**
   * ----------------------Dispatcher level----------------------
   */
  uint8_t kDispQueueNumber = 1;
  /// Minimal number of buffered packets for collect_tx_pkts
  size_t kTxBatchSize = 32;
  /// Maximum number of transmitted packets for tx_burst
  size_t kTxPostSize = 32;
  /// Minimal number of buffered packets before dispatching
  size_t kRxBatchSize = 32;
  /// Maximum number of packets received in rx_burst
  static constexpr size_t kRxPostSize = 128;
};


} // namespace dperf