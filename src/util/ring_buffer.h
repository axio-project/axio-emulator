#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#define AXIO_COMPILER_BARRIER() __asm__ __volatile__("" : : : "memory")

namespace axio {

/** Single-producer, single-consumer view over caller-owned storage. */
class RingBuffer {
 public:
  RingBuffer(void* buffer, size_t size, size_t element_size,
             uint64_t* input_index, uint64_t* output_index)
      : data_(static_cast<uint8_t*>(buffer)),
        mask_(size / element_size - 1),
        size_(size / element_size),
        element_size_(element_size),
        input_index_(input_index),
        output_index_(output_index) {}

  int copy_in(const void* source, size_t length) {
    size_t available_length = this->unused_length();
    if (length > available_length) {
      return -1;
    }

    uint64_t offset = *this->input_index_;
    this->_copy_in(source, length, offset);
    *this->input_index_ += length;
    return length;
  }

  int copy_out(void* destination, size_t length) {
    size_t available_length = *this->input_index_ - *this->output_index_;
    if (length > available_length) {
      return -1;
    }

    uint64_t offset = *this->output_index_;
    this->_copy_out(destination, length, offset);
    *this->output_index_ += length;
    return length;
  }

  bool empty() const { return *this->input_index_ == *this->output_index_; }
  bool full() const { return this->unused_length() == 0; }

  size_t unused_length() const {
    return (this->mask_ + 1) -
           (*this->input_index_ - *this->output_index_);
  }

  size_t used_length() const {
    return *this->input_index_ - *this->output_index_;
  }

 private:
  void _copy_in(const void* source, size_t length, uint64_t offset) {
    offset &= this->mask_;
    if (this->element_size_ != 1) {
      offset *= this->element_size_;
      this->size_ *= this->element_size_;
      length *= this->element_size_;
    }

    size_t first_length =
        std::min(length, this->size_ - static_cast<size_t>(offset));
    auto* source_bytes = static_cast<const uint8_t*>(source);
    std::memcpy(this->data_ + offset, source_bytes, first_length);
    std::memcpy(this->data_, source_bytes + first_length,
                length - first_length);
    AXIO_COMPILER_BARRIER();
  }

  void _copy_out(void* destination, size_t length, uint64_t offset) {
    if (this->element_size_ != 1) {
      offset *= this->element_size_;
      this->size_ *= this->element_size_;
      length *= this->element_size_;
    }

    size_t first_length =
        std::min(length, this->size_ - static_cast<size_t>(offset));
    auto* destination_bytes = static_cast<uint8_t*>(destination);
    std::memcpy(destination_bytes, this->data_ + offset, first_length);
    std::memcpy(destination_bytes + first_length, this->data_,
                length - first_length);
    AXIO_COMPILER_BARRIER();
  }

  uint8_t* data_;
  size_t mask_;
  size_t size_;
  size_t element_size_;
  uint64_t* input_index_;
  uint64_t* output_index_;
};

}  // namespace axio
