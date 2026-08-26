/**
 * @file workload_semantics.h
 * @brief Deterministic workload primitives shared by Axio and AE tests.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace axio::workloads {

class DeterministicRandom {
 public:
  explicit DeterministicRandom(uint64_t seed) : state_(seed) {
    if (seed == 0) throw std::invalid_argument("random seed must be positive");
  }

  uint64_t next() {
    this->state_ ^= this->state_ >> 12;
    this->state_ ^= this->state_ << 25;
    this->state_ ^= this->state_ >> 27;
    return this->state_ * 2685821657736338717ULL;
  }

 private:
  uint64_t state_;
};

class MemoryWorkload {
 public:
  MemoryWorkload(size_t state_bytes, size_t access_bytes, uint64_t seed)
      : state_bytes_(state_bytes),
        access_bytes_(access_bytes),
        random_(seed) {
    if (state_bytes == 0 || access_bytes == 0 || access_bytes > state_bytes ||
        state_bytes % access_bytes != 0) {
      throw std::invalid_argument(
          "memory workload requires evenly divisible non-zero regions");
    }
  }

  size_t read_modify_write(uint8_t* state) {
    const size_t block_count = this->state_bytes_ / this->access_bytes_;
    const size_t offset =
        static_cast<size_t>(this->random_.next() % block_count) *
        this->access_bytes_;
    uint64_t checksum = this->checksum_;
    for (size_t index = 0; index < this->access_bytes_; ++index) {
      const uint8_t original = state[offset + index];
      checksum = (checksum * 131U) ^ original;
      state[offset + index] = static_cast<uint8_t>(original + 1U);
    }
    this->checksum_ = checksum;
    return offset;
  }

  uint64_t checksum() const { return this->checksum_; }

 private:
  size_t state_bytes_;
  size_t access_bytes_;
  DeterministicRandom random_;
  uint64_t checksum_ = 0;
};

struct KeyValueShard {
  size_t first;
  size_t size;
};

inline KeyValueShard key_value_shard(size_t entry_count, size_t shard_count,
                                     size_t shard_index) {
  if (shard_count == 0 || shard_index >= shard_count) {
    throw std::invalid_argument("invalid key-value shard index");
  }
  const size_t base = entry_count / shard_count;
  const size_t extra = entry_count % shard_count;
  const size_t size = base + (shard_index < extra ? 1 : 0);
  const size_t first = shard_index * base + std::min(shard_index, extra);
  return {first, size};
}

enum class KeyValueOperation : uint8_t {
  kPut = 0,
  kGet = 1,
};

class DeterministicOperationMix {
 public:
  explicit DeterministicOperationMix(double get_ratio) {
    if (!std::isfinite(get_ratio) || get_ratio < 0.0 || get_ratio > 1.0) {
      throw std::invalid_argument("get ratio must be between zero and one");
    }
    this->get_units_ = static_cast<uint64_t>(
        std::llround(get_ratio * static_cast<double>(kScale)));
  }

  KeyValueOperation next() {
    this->accumulator_ += this->get_units_;
    if (this->accumulator_ >= kScale) {
      this->accumulator_ -= kScale;
      return KeyValueOperation::kGet;
    }
    return KeyValueOperation::kPut;
  }

 private:
  static constexpr uint64_t kScale = 1000000;
  uint64_t get_units_ = 0;
  uint64_t accumulator_ = 0;
};

}  // namespace axio::workloads
