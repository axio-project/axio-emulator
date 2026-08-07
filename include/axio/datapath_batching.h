/**
 * @file datapath_batching.h
 * @brief Backend-independent batching policy for dispatcher and NIC stages.
 */
#pragma once

#include <algorithm>
#include <cstddef>

namespace axio {

inline bool dispatcher_batch_ready(size_t pending_count,
                                   size_t batch_size) {
  return pending_count >= batch_size;
}

inline size_t nic_post_count(size_t pending_count, size_t post_size) {
  return std::min(pending_count, post_size);
}

}  // namespace axio
