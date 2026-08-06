#pragma once

#include "util/ring_buffer.h"

#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/interprocess_mutex.hpp>

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace axio {

/**
 * Shared-memory transport for one writer and one reader of uint64_t vectors.
 *
 * The data ring is followed by read/write indexes and synchronization objects
 * in a fixed-size control region.
 */
class SharedMemoryBuffer {
 public:
  static constexpr size_t kDefaultBufferSize = 1 << 20;
  static constexpr size_t kDefaultControlRegionSize = 400;

  SharedMemoryBuffer(
      const std::string& name, bool create = false,
      size_t size = kDefaultBufferSize,
      boost::interprocess::mode_t mode = boost::interprocess::read_write) {
    this->name_ = name;
    this->create_ = create;
    uint64_t total_size;
    try {
      if (!create) {
        this->shared_memory_ = boost::interprocess::shared_memory_object(
            boost::interprocess::open_only, this->name_.c_str(), mode);
        this->region_ = boost::interprocess::mapped_region(
            this->shared_memory_, boost::interprocess::read_write);
        total_size = this->region_.get_size();
      } else {
        this->shared_memory_ = boost::interprocess::shared_memory_object(
            boost::interprocess::open_or_create, this->name_.c_str(), mode);
        this->data_region_size_ = size;
        if (!(size != 0 && (size & (size - 1)) == 0)) {
          int exponent = std::ceil(std::log2(size));
          this->data_region_size_ = 1 << exponent;
        }
        total_size =
            this->data_region_size_ + this->control_region_byte_size_;
        this->shared_memory_.truncate(total_size);
        this->region_ = boost::interprocess::mapped_region(
            this->shared_memory_, boost::interprocess::read_write);
      }
    } catch (const boost::interprocess::interprocess_exception& exception) {
      std::cerr << "Axio: failed to create or open shared memory object "
                << this->name_ << ": " << exception.what() << std::endl;
      throw;
    }

    this->buffer_ =
        reinterpret_cast<uint64_t*>(this->region_.get_address());
    this->data_buffer_ = this->buffer_;
    this->data_region_size_ =
        total_size - this->control_region_byte_size_;
    this->control_start_ = this->data_region_size_ / sizeof(uint64_t);
    this->read_index_ = &this->buffer_[this->control_start_];
    this->write_index_ = &this->buffer_[this->control_start_ + 1];

    if (this->create_) {
      *this->read_index_ = 0;
      *this->write_index_ = 0;
      this->mutex_ = new (&this->buffer_[this->control_start_ + 2])
          boost::interprocess::interprocess_mutex;
      this->full_condition_ =
          new (&this->buffer_[this->control_start_ + 2 +
                              sizeof(boost::interprocess::interprocess_mutex) /
                                  sizeof(uint64_t)])
              boost::interprocess::interprocess_condition;
      this->empty_condition_ =
          new (&this->buffer_[this->control_start_ + 2 +
                              sizeof(boost::interprocess::interprocess_mutex) /
                                  sizeof(uint64_t) +
                              sizeof(
                                  boost::interprocess::interprocess_condition) /
                                  sizeof(uint64_t)])
              boost::interprocess::interprocess_condition;
    } else {
      this->mutex_ =
          reinterpret_cast<boost::interprocess::interprocess_mutex*>(
              &this->buffer_[this->control_start_ + 2]);
      this->full_condition_ =
          reinterpret_cast<boost::interprocess::interprocess_condition*>(
              &this->buffer_[this->control_start_ + 2 +
                             sizeof(boost::interprocess::interprocess_mutex) /
                                 sizeof(uint64_t)]);
      this->empty_condition_ =
          reinterpret_cast<boost::interprocess::interprocess_condition*>(
              &this->buffer_[this->control_start_ + 2 +
                             sizeof(boost::interprocess::interprocess_mutex) /
                                 sizeof(uint64_t) +
                             sizeof(
                                 boost::interprocess::interprocess_condition) /
                                 sizeof(uint64_t)]);
    }

    this->ring_buffer_ =
        new RingBuffer(static_cast<void*>(this->data_buffer_),
                       this->data_region_size_, sizeof(uint64_t),
                       this->read_index_, this->write_index_);
  }

  ~SharedMemoryBuffer() {
    if (this->create_) {
      boost::interprocess::shared_memory_object::remove(this->name_.c_str());
    }
  }

  int send_data(const std::vector<uint64_t>& data) {
    uint64_t element_count = data.size();
    if (this->ring_buffer_->unused_length() < element_count + 1) {
      return -1;
    }

    int result = this->ring_buffer_->copy_in(&element_count, 1);
    assert(result == 1);
    result = this->ring_buffer_->copy_in(data.data(), data.size());
    assert(result > 0 && static_cast<size_t>(result) == element_count);
    return element_count;
  }

  std::vector<uint64_t> receive_data() {
    std::vector<uint64_t> data;
    uint64_t element_count = 0;
    int result = this->ring_buffer_->copy_out(&element_count, 1);
    if (result < 0) {
      return data;
    }

    data.resize(element_count);
    size_t total_length = 0;
    while (true) {
      result = this->ring_buffer_->copy_out(
          data.data() + total_length, element_count - total_length);
      if (result < 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        continue;
      }
      total_length += result;
      if (total_length == element_count) {
        break;
      }
    }
    return data;
  }

  size_t queue_length() const { return this->ring_buffer_->used_length(); }

 private:
  std::string name_;
  boost::interprocess::shared_memory_object shared_memory_;
  boost::interprocess::mapped_region region_;
  bool create_;
  uint64_t* buffer_;
  uint64_t* data_buffer_;
  RingBuffer* ring_buffer_;
  uint64_t* read_index_;
  uint64_t* write_index_;
  uint64_t control_start_;
  uint64_t control_region_byte_size_ = kDefaultControlRegionSize;
  uint64_t data_region_size_;
  boost::interprocess::interprocess_mutex* mutex_;
  boost::interprocess::interprocess_condition* full_condition_;
  boost::interprocess::interprocess_condition* empty_condition_;
};

}  // namespace axio
