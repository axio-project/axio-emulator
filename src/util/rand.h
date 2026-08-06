#pragma once

#include <cstdint>
#include <random>

namespace axio {

class SlowRandom {
 public:
  SlowRandom()
      : engine_(this->random_device_()), distribution_(0, UINT64_MAX) {}

  uint64_t next_u64() { return this->distribution_(this->engine_); }

 private:
  std::random_device random_device_;
  std::mt19937_64 engine_;
  std::uniform_int_distribution<uint64_t> distribution_;
};

class FastRandom {
 public:
  FastRandom() {
    SlowRandom slow_random;
    this->seed_ = slow_random.next_u64();
  }

  uint64_t next_u64() {
    this->_advance();
    return this->seed_;
  }

  uint32_t next_u32() {
    this->_advance();
    return static_cast<uint32_t>(this->seed_ >> 32);
  }

  uint16_t next_u16() {
    this->_advance();
    return static_cast<uint16_t>(this->seed_ >> 48);
  }

  uint8_t next_u8() {
    this->_advance();
    return static_cast<uint8_t>(this->seed_ >> 56);
  }

 private:
  void _advance() { this->seed_ = this->seed_ * 1103515245 + 12345; }

  uint64_t seed_;
};

}  // namespace axio
