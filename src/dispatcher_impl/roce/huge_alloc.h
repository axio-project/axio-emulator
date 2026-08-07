#pragma once

#include "buffer.h"
#include "common.h"
#include "util/logger.h"
#include "util/math_utils.h"
#include "util/rand.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace axio {

/** Passive record for a shared-memory region owned by HugeAlloc. */
struct SharedMemoryRegion {
  const int key_;
  const uint8_t* buffer_;
  const size_t size_;
  const bool registered_;

  SharedMemoryRegion(int key, uint8_t* buffer, size_t size, bool registered)
      : key_(key), buffer_(buffer), size_(size), registered_(registered) {
    assert(size % kHugepageSize == 0);
  }
};

enum class MemoryRegistration { kEnabled, kDisabled };

/**
 * Hugepage allocator backed by initialization-time buddy freelists and an
 * explicitly prepared lock-free pool for the fixed-size datapath buffers.
 *
 * The allocator owns its shared-memory regions and Buffer descriptors. Buffer
 * payload storage remains valid until the allocator is destroyed.
 */
class HugeAlloc {
 public:
  static constexpr const char* kAllocationFailureHelp =
      "This could be due to insufficient huge pages or SHM limits.";
  static constexpr size_t kMinClassSize = 64;
  static constexpr size_t kMinClassBitShift = 6;
  static_assert((kMinClassSize >> kMinClassBitShift) == 1, "");

  static constexpr size_t kMaxClassSize = AXIO_MB(8);
  static constexpr size_t kNumClasses = 18;
  static_assert(kMaxClassSize == kMinClassSize << (kNumClasses - 1), "");

  static constexpr size_t max_class_size(size_t class_index) {
    return kMinClassSize * (1ull << class_index);
  }

  HugeAlloc(size_t initial_size, size_t numa_node);
  ~HugeAlloc();

  Buffer allocate_raw(size_t size, MemoryRegistration registration);
  Buffer* allocate(size_t size);
  bool allocate_bulk(size_t size, Buffer** buffers, size_t count);
  void prepare_reusable_pool(size_t size, bool concurrent_access);
  void add_raw_buffer(Buffer buffer, size_t size);
  void free_buffer(Buffer* buffer);
  void free_buffers(Buffer* const* buffers, size_t count);

  size_t numa_node() const { return this->numa_node_; }

  size_t reserved_bytes() const {
    const std::lock_guard<std::mutex> lock(this->mutex_);
    assert(this->stats_.shared_memory_reserved_ % kHugepageSize == 0);
    return this->stats_.shared_memory_reserved_;
  }

  size_t user_allocated_bytes() const {
    const size_t allocated =
        this->stats_.user_allocated_.load(std::memory_order_relaxed);
    assert(allocated % kMinClassSize == 0);
    return allocated;
  }

  void print_statistics();

 private:
  class ReusableBufferPool;

  struct AllocatorStats {
    size_t shared_memory_reserved_ = 0;
    std::atomic<size_t> user_allocated_{0};
  };

  inline size_t _class_index(size_t size) {
#ifdef _WIN32
    return this->_class_index_slow(size);
#else
    assert(size >= 1 && size <= kMaxClassSize);
    return msb_index(
        static_cast<int>((size - 1) >> kMinClassBitShift));
#endif
  }

  inline size_t _class_index_slow(size_t size) {
    assert(size >= 1 && size <= kMaxClassSize);

    size_t class_index = 0;
    size_t class_limit = kMinClassSize;
    while (size > class_limit) {
      class_index++;
      class_limit *= 2;
    }
    return class_index;
  }

  inline void _split_class(size_t class_index) {
    assert(class_index >= 1);
    assert(!this->free_lists_[class_index].empty());

    Buffer* buffer = this->free_lists_[class_index].back();
    this->free_lists_[class_index].pop_back();
    assert(buffer->class_size_ == max_class_size(class_index));

    Buffer* first =
        new Buffer(buffer->buf_, buffer->class_size_ / 2, buffer->lkey_);
    Buffer* second =
        new Buffer(buffer->buf_ + buffer->class_size_ / 2,
                   buffer->class_size_ / 2, buffer->lkey_);
    delete buffer;

    this->free_lists_[class_index - 1].push_back(first);
    this->free_lists_[class_index - 1].push_back(second);
  }

  inline Buffer* _allocate_from_class(size_t class_index) {
    assert(class_index < kNumClasses);

    Buffer* buffer = this->free_lists_[class_index].back();
    assert(buffer->class_size_ == max_class_size(class_index));
    this->free_lists_[class_index].pop_back();
    this->stats_.user_allocated_.fetch_add(buffer->class_size_,
                                          std::memory_order_relaxed);
    return buffer;
  }

  Buffer* _allocate_locked(size_t size);
  void _free_buffer_locked(Buffer* buffer);

  bool _reserve_hugepages(size_t size);

  std::vector<SharedMemoryRegion> shared_memory_regions_;
  std::vector<Buffer*> free_lists_[kNumClasses];
  std::unique_ptr<ReusableBufferPool> reusable_pools_[kNumClasses];
  SlowRandom random_;
  const size_t numa_node_;
  size_t previous_allocation_size_;
  mutable std::mutex mutex_;
  AllocatorStats stats_;
};

}  // namespace axio
