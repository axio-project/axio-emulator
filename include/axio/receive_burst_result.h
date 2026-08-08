/**
 * @file receive_burst_result.h
 * @brief Backend-independent result of one NIC RX poll.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace axio {

struct ReceiveBurstResult {
  size_t successful_count = 0;
  size_t error_count = 0;
  uint64_t completion_tsc = 0;
  uint32_t completion_cpu_id = 0;
};

}  // namespace axio
