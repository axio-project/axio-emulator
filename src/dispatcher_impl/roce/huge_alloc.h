#pragma once

#include "common.h"
#include "util/rand.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace axio {

/** A non-owning slice of memory registered with the RoCE device. */
struct RegisteredMemorySlice {
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
  uint32_t lkey_ = UINT32_MAX;

  explicit operator bool() const { return this->data_ != nullptr; }
};

/**
 * Own hugepage mappings and partition registered memory during initialization.
 *
 * HugeAlloc never participates in buffer allocation or release after the
 * dispatcher has constructed its RX ring and RoceBufferPool.
 */
class HugeAlloc {
 public:
  static constexpr const char* kAllocationFailureHelp =
      "This could be due to insufficient huge pages or SHM limits.";
  static constexpr size_t kMinClassSize = 64;
  static constexpr size_t kMaxClassSize = AXIO_MB(8);

  explicit HugeAlloc(size_t numa_node);
  ~HugeAlloc();

  HugeAlloc(const HugeAlloc&) = delete;
  HugeAlloc& operator=(const HugeAlloc&) = delete;

  /** Reserve an owned hugepage-backed mapping. The returned lkey is invalid. */
  RegisteredMemorySlice reserve(size_t size);

  /** Add registration metadata for a region that this allocator may slice. */
  void add_registered_region(RegisteredMemorySlice region);

  /** Allocate one aligned slice. Returns an invalid slice on exhaustion. */
  RegisteredMemorySlice allocate(size_t size,
                                 size_t alignment = kMinClassSize);

  /** Consume every remaining complete fixed-size slice. */
  std::vector<RegisteredMemorySlice> take_remaining_fixed_slices(
      size_t slice_size);

  size_t numa_node() const { return this->numa_node_; }
  size_t reserved_bytes() const { return this->reserved_bytes_; }
  size_t user_allocated_bytes() const { return this->allocated_bytes_; }

  void print_statistics() const;

 private:
  struct OwnedMemoryRegion {
    int key_;
    uint8_t* data_;
    size_t size_;
    bool system_v_;
  };

  struct RegisteredRegion {
    uint8_t* data_;
    size_t size_;
    uint32_t lkey_;
    size_t next_offset_ = 0;
  };

  static size_t _aligned_offset(const RegisteredRegion& region,
                                size_t alignment);

  std::vector<OwnedMemoryRegion> owned_regions_;
  std::vector<RegisteredRegion> registered_regions_;
  SlowRandom random_;
  const size_t numa_node_;
  size_t reserved_bytes_ = 0;
  size_t allocated_bytes_ = 0;
};

}  // namespace axio
