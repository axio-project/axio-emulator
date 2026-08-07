/**
 * @file runtime_limits.h
 * @brief Fixed storage limits shared by config validation and the datapath.
 */
#pragma once

#include <cstdint>

namespace axio::config {

inline constexpr uint32_t kMaximumApplicationBatchSize = 512;
inline constexpr uint32_t kApplicationQueueEntries = 4096;

}  // namespace axio::config
