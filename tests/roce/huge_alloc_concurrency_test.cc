#include "dispatcher_impl/roce/huge_alloc.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr size_t kBufferSize = 4096;
constexpr size_t kReceiveExtentSize = axio::HugeAlloc::kMaxClassSize;
constexpr size_t kRegionSize = 2 * kReceiveExtentSize;
constexpr uint32_t kLocalKey = 17;

bool slices_overlap(const axio::RegisteredMemorySlice& first,
                    const axio::RegisteredMemorySlice& second) {
  const uintptr_t first_begin = reinterpret_cast<uintptr_t>(first.data_);
  const uintptr_t first_end = first_begin + first.size_;
  const uintptr_t second_begin = reinterpret_cast<uintptr_t>(second.data_);
  const uintptr_t second_end = second_begin + second.size_;
  return first_begin < second_end && second_begin < first_end;
}

bool exercise_registered_slices() {
  auto* storage = static_cast<uint8_t*>(
      std::aligned_alloc(kBufferSize, kRegionSize));
  if (storage == nullptr) return false;

  bool passed = true;
  {
    axio::HugeAlloc allocator(0);
    allocator.add_registered_region(
        {storage, kRegionSize, kLocalKey});

    const axio::RegisteredMemorySlice receive_extent =
        allocator.allocate(kReceiveExtentSize, kBufferSize);
    passed = passed && receive_extent.data_ == storage;
    passed = passed && receive_extent.size_ == kReceiveExtentSize;
    passed = passed && receive_extent.lkey_ == kLocalKey;

    const std::vector<axio::RegisteredMemorySlice> pool_slices =
        allocator.take_remaining_fixed_slices(kBufferSize);
    passed = passed &&
             pool_slices.size() ==
                 (kRegionSize - kReceiveExtentSize) / kBufferSize;
    for (size_t index = 0; index < pool_slices.size(); ++index) {
      const axio::RegisteredMemorySlice& slice = pool_slices[index];
      passed = passed && slice.data_ ==
                             storage + kReceiveExtentSize +
                                 (index * kBufferSize);
      passed = passed && slice.size_ == kBufferSize;
      passed = passed && slice.lkey_ == kLocalKey;
      passed = passed && !slices_overlap(receive_extent, slice);
    }

    const axio::RegisteredMemorySlice exhausted =
        allocator.allocate(kBufferSize, kBufferSize);
    passed = passed && exhausted.data_ == nullptr;
    passed = passed &&
             allocator.user_allocated_bytes() == kRegionSize;
  }
  std::free(storage);
  return passed;
}

}  // namespace

int main() {
  if (!exercise_registered_slices()) {
    std::fprintf(stderr, "HugeAlloc registered-slice test failed\n");
    return 1;
  }
  std::puts("HugeAlloc registered-slice test passed");
  return 0;
}
