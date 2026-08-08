#pragma once

#include "buffer.h"
#include "huge_alloc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace axio {

enum class RoceBufferPoolAccess { kLocal, kShared };

/** Fixed-size reusable RoCE buffers backed by one registered HugeAlloc region. */
class RoceBufferPool {
 public:
  RoceBufferPool(HugeAlloc* allocator, size_t buffer_size,
                 RoceBufferPoolAccess access);

  RoceBufferPool(const RoceBufferPool&) = delete;
  RoceBufferPool& operator=(const RoceBufferPool&) = delete;

  Buffer* allocate() { return this->operations_->allocate_(this); }

  bool allocate_bulk(Buffer** buffers, size_t count) {
    return this->operations_->allocate_bulk_(this, buffers, count);
  }

  void free(Buffer* buffer) {
    this->operations_->free_(this, buffer);
  }

  void free_bulk(Buffer* const* buffers, size_t count) {
    this->operations_->free_bulk_(this, buffers, count);
  }

  bool owns(const Buffer* buffer) const;
  size_t capacity() const { return this->capacity_; }

  size_t allocated_count() const {
    return this->operations_->allocated_count_(this);
  }

 private:
  static constexpr uint8_t kAvailable = 0;
  static constexpr uint8_t kAllocated = 1;

  struct Operations {
    Buffer* (*allocate_)(RoceBufferPool*);
    bool (*allocate_bulk_)(RoceBufferPool*, Buffer**, size_t);
    void (*free_)(RoceBufferPool*, Buffer*);
    void (*free_bulk_)(RoceBufferPool*, Buffer* const*, size_t);
    size_t (*allocated_count_)(const RoceBufferPool*);
  };

  static Buffer* _allocate_local(RoceBufferPool* pool);
  static bool _allocate_local_bulk(RoceBufferPool* pool, Buffer** buffers,
                                   size_t count);
  static void _free_local(RoceBufferPool* pool, Buffer* buffer);
  static void _free_local_bulk(RoceBufferPool* pool,
                               Buffer* const* buffers, size_t count);
  static size_t _local_allocated_count(const RoceBufferPool* pool);

  static Buffer* _allocate_shared(RoceBufferPool* pool);
  static bool _allocate_shared_bulk(RoceBufferPool* pool, Buffer** buffers,
                                    size_t count);
  static void _free_shared(RoceBufferPool* pool, Buffer* buffer);
  static void _free_shared_bulk(RoceBufferPool* pool,
                                Buffer* const* buffers, size_t count);
  static size_t _shared_allocated_count(const RoceBufferPool* pool);

  size_t _index(const Buffer* buffer) const;

  static const Operations kLocalOperations;
  static const Operations kSharedOperations;

  const Operations* operations_;
  const size_t buffer_size_;
  size_t capacity_ = 0;
  std::unique_ptr<Buffer[]> buffers_;

  std::vector<Buffer*> local_free_buffers_;
  std::unique_ptr<uint8_t[]> local_state_;
  size_t local_allocated_count_ = 0;

  std::unique_ptr<std::atomic<uint8_t>[]> shared_state_;
  std::atomic<size_t> shared_next_index_{0};
  std::atomic<size_t> shared_allocated_count_{0};
};

}  // namespace axio
