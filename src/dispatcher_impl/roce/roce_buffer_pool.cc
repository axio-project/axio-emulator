#include "roce_buffer_pool.h"

#include <cassert>
#include <stdexcept>

namespace axio {

const RoceBufferPool::Operations RoceBufferPool::kLocalOperations = {
    &RoceBufferPool::_allocate_local,
    &RoceBufferPool::_allocate_local_bulk,
    &RoceBufferPool::_free_local,
    &RoceBufferPool::_free_local_bulk,
    &RoceBufferPool::_local_allocated_count};

const RoceBufferPool::Operations RoceBufferPool::kSharedOperations = {
    &RoceBufferPool::_allocate_shared,
    &RoceBufferPool::_allocate_shared_bulk,
    &RoceBufferPool::_free_shared,
    &RoceBufferPool::_free_shared_bulk,
    &RoceBufferPool::_shared_allocated_count};

RoceBufferPool::RoceBufferPool(HugeAlloc* allocator, size_t buffer_size,
                               RoceBufferPoolAccess access)
    : operations_(access == RoceBufferPoolAccess::kLocal
                      ? &kLocalOperations
                      : &kSharedOperations),
      buffer_size_(buffer_size) {
  if (allocator == nullptr || buffer_size == 0) {
    throw std::invalid_argument(
        "RoceBufferPool requires an allocator and nonzero buffer size");
  }

  std::vector<RegisteredMemorySlice> slices =
      allocator->take_remaining_fixed_slices(buffer_size);
  if (slices.empty()) {
    throw std::runtime_error("RoceBufferPool has no registered backing slices");
  }

  this->capacity_ = slices.size();
  this->buffers_ = std::make_unique<Buffer[]>(this->capacity_);
  for (size_t index = 0; index < this->capacity_; ++index) {
    const RegisteredMemorySlice& slice = slices[index];
    assert(slice.size_ == this->buffer_size_);
    this->buffers_[index] =
        Buffer(slice.data_, slice.size_, slice.lkey_);
  }

  if (access == RoceBufferPoolAccess::kLocal) {
    this->local_state_ = std::make_unique<uint8_t[]>(this->capacity_);
    this->local_free_buffers_.reserve(this->capacity_);
    for (size_t index = 0; index < this->capacity_; ++index) {
      this->local_state_[index] = kAvailable;
      this->local_free_buffers_.push_back(&this->buffers_[index]);
    }
  } else {
    this->shared_state_ =
        std::make_unique<std::atomic<uint8_t>[]>(this->capacity_);
    for (size_t index = 0; index < this->capacity_; ++index) {
      this->shared_state_[index].store(kAvailable, std::memory_order_relaxed);
    }
  }
}

bool RoceBufferPool::owns(const Buffer* buffer) const {
  if (buffer == nullptr || this->capacity_ == 0) return false;
  const uintptr_t address = reinterpret_cast<uintptr_t>(buffer);
  const uintptr_t begin = reinterpret_cast<uintptr_t>(this->buffers_.get());
  const uintptr_t end = begin + (this->capacity_ * sizeof(Buffer));
  return address >= begin && address < end;
}

size_t RoceBufferPool::_index(const Buffer* buffer) const {
  assert(this->owns(buffer));
  return static_cast<size_t>(buffer - this->buffers_.get());
}

Buffer* RoceBufferPool::_allocate_local(RoceBufferPool* pool) {
  if (pool->local_free_buffers_.empty()) return nullptr;
  Buffer* buffer = pool->local_free_buffers_.back();
  pool->local_free_buffers_.pop_back();
  const size_t index = pool->_index(buffer);
  assert(pool->local_state_[index] == kAvailable);
  pool->local_state_[index] = kAllocated;
  ++pool->local_allocated_count_;
  return buffer;
}

bool RoceBufferPool::_allocate_local_bulk(RoceBufferPool* pool,
                                          Buffer** buffers, size_t count) {
  if (count == 0) return true;
  assert(buffers != nullptr);
  for (size_t index = 0; index < count; ++index) buffers[index] = nullptr;
  if (count > pool->local_free_buffers_.size()) return false;

  for (size_t index = 0; index < count; ++index) {
    Buffer* buffer = pool->local_free_buffers_.back();
    pool->local_free_buffers_.pop_back();
    const size_t pool_index = pool->_index(buffer);
    assert(pool->local_state_[pool_index] == kAvailable);
    pool->local_state_[pool_index] = kAllocated;
    buffers[index] = buffer;
  }
  pool->local_allocated_count_ += count;
  return true;
}

void RoceBufferPool::_free_local(RoceBufferPool* pool, Buffer* buffer) {
  const size_t index = pool->_index(buffer);
  assert(pool->local_state_[index] == kAllocated);
  assert(pool->local_allocated_count_ != 0);
  buffer->length_ = 0;
  pool->local_state_[index] = kAvailable;
  pool->local_free_buffers_.push_back(buffer);
  --pool->local_allocated_count_;
}

void RoceBufferPool::_free_local_bulk(RoceBufferPool* pool,
                                      Buffer* const* buffers, size_t count) {
  if (count == 0) return;
  assert(buffers != nullptr);
  assert(pool->local_allocated_count_ >= count);
  for (size_t index = 0; index < count; ++index) {
    Buffer* buffer = buffers[index];
    const size_t pool_index = pool->_index(buffer);
    assert(pool->local_state_[pool_index] == kAllocated);
    buffer->length_ = 0;
    pool->local_state_[pool_index] = kAvailable;
    pool->local_free_buffers_.push_back(buffer);
  }
  pool->local_allocated_count_ -= count;
  assert(pool->local_free_buffers_.size() <= pool->capacity_);
}

size_t RoceBufferPool::_local_allocated_count(const RoceBufferPool* pool) {
  return pool->local_allocated_count_;
}

Buffer* RoceBufferPool::_allocate_shared(RoceBufferPool* pool) {
  const size_t start =
      pool->shared_next_index_.fetch_add(1, std::memory_order_relaxed);
  size_t index = start % pool->capacity_;
  for (size_t offset = 0; offset < pool->capacity_; ++offset) {
    uint8_t expected = kAvailable;
    if (pool->shared_state_[index].compare_exchange_strong(
            expected, kAllocated, std::memory_order_acquire,
            std::memory_order_relaxed)) {
      pool->shared_allocated_count_.fetch_add(1, std::memory_order_relaxed);
      return &pool->buffers_[index];
    }
    if (++index == pool->capacity_) index = 0;
  }
  return nullptr;
}

bool RoceBufferPool::_allocate_shared_bulk(RoceBufferPool* pool,
                                           Buffer** buffers, size_t count) {
  if (count == 0) return true;
  assert(buffers != nullptr);
  for (size_t index = 0; index < count; ++index) buffers[index] = nullptr;
  if (count > pool->capacity_) return false;

  const size_t start =
      pool->shared_next_index_.fetch_add(count, std::memory_order_relaxed);
  size_t allocated_count = 0;
  size_t index = start % pool->capacity_;
  for (size_t offset = 0;
       offset < pool->capacity_ && allocated_count < count; ++offset) {
    uint8_t expected = kAvailable;
    if (pool->shared_state_[index].compare_exchange_strong(
            expected, kAllocated, std::memory_order_acquire,
            std::memory_order_relaxed)) {
      buffers[allocated_count++] = &pool->buffers_[index];
    }
    if (++index == pool->capacity_) index = 0;
  }
  if (allocated_count == count) {
    pool->shared_allocated_count_.fetch_add(count,
                                            std::memory_order_relaxed);
    return true;
  }

  while (allocated_count != 0) {
    Buffer* buffer = buffers[--allocated_count];
    const size_t pool_index = pool->_index(buffer);
    pool->shared_state_[pool_index].store(kAvailable,
                                          std::memory_order_release);
    buffers[allocated_count] = nullptr;
  }
  return false;
}

void RoceBufferPool::_free_shared(RoceBufferPool* pool, Buffer* buffer) {
  const size_t index = pool->_index(buffer);
  assert(pool->shared_state_[index].load(std::memory_order_relaxed) ==
         kAllocated);
  buffer->length_ = 0;
  pool->shared_state_[index].store(kAvailable, std::memory_order_release);
  const size_t previous =
      pool->shared_allocated_count_.fetch_sub(1, std::memory_order_relaxed);
  assert(previous != 0);
}

void RoceBufferPool::_free_shared_bulk(RoceBufferPool* pool,
                                       Buffer* const* buffers, size_t count) {
  if (count == 0) return;
  assert(buffers != nullptr);
  for (size_t index = 0; index < count; ++index) {
    Buffer* buffer = buffers[index];
    const size_t pool_index = pool->_index(buffer);
    assert(pool->shared_state_[pool_index].load(std::memory_order_relaxed) ==
           kAllocated);
    buffer->length_ = 0;
    pool->shared_state_[pool_index].store(kAvailable,
                                          std::memory_order_release);
  }
  const size_t previous = pool->shared_allocated_count_.fetch_sub(
      count, std::memory_order_relaxed);
  assert(previous >= count);
}

size_t RoceBufferPool::_shared_allocated_count(
    const RoceBufferPool* pool) {
  return pool->shared_allocated_count_.load(std::memory_order_relaxed);
}

}  // namespace axio
