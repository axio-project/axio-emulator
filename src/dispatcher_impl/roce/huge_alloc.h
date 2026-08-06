#pragma once

#include "buffer.h"
#include "common.h"
#include "util/logger.h"
#include "util/math_utils.h"
#include "util/rand.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
 * Hugepage allocator backed by per-class freelists.
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
  void add_raw_buffer(Buffer buffer, size_t size);

  inline void free_buffer(Buffer* buffer) {
    assert(buffer->buf_ != nullptr);
    buffer->length_ = 0;
    buffer->state_ = Buffer::kFree;

    size_t class_index = this->_class_index(buffer->class_size_);
    assert(max_class_size(class_index) == buffer->class_size_);

    this->free_lists_[class_index].push_back(buffer);
    this->stats_.user_allocated_ -= buffer->class_size_;
  }

  size_t numa_node() const { return this->numa_node_; }

  size_t reserved_bytes() const {
    assert(this->stats_.shared_memory_reserved_ % kHugepageSize == 0);
    return this->stats_.shared_memory_reserved_;
  }

  size_t user_allocated_bytes() const {
    assert(this->stats_.user_allocated_ % kMinClassSize == 0);
    return this->stats_.user_allocated_;
  }

  void print_statistics();

 private:
  struct AllocatorStats {
    size_t shared_memory_reserved_ = 0;
    size_t user_allocated_ = 0;
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
    assert(this->free_lists_[class_index - 1].empty());

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
    this->stats_.user_allocated_ += buffer->class_size_;
    return buffer;
  }

  bool _reserve_hugepages(size_t size);

  std::vector<SharedMemoryRegion> shared_memory_regions_;
  std::vector<Buffer*> free_lists_[kNumClasses];
  SlowRand random_;
  const size_t numa_node_;
  size_t previous_allocation_size_;
  AllocatorStats stats_;
};

}  // namespace axio
