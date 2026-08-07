#include "huge_alloc.h"

#include "util/logger.h"

#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>

#ifdef __linux__
#include <numaif.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

namespace axio {

namespace {

size_t round_up_to_multiple(size_t value, size_t alignment) {
  assert(alignment != 0);
  const size_t remainder = value % alignment;
  if (remainder == 0) return value;
  const size_t increment = alignment - remainder;
  if (value > std::numeric_limits<size_t>::max() - increment) return 0;
  return value + increment;
}

}  // namespace

HugeAlloc::HugeAlloc(size_t numa_node) : numa_node_(numa_node) {
  assert(numa_node <= kMaxNumaNodes);
}

HugeAlloc::~HugeAlloc() {
  for (const OwnedMemoryRegion& region : this->owned_regions_) {
#ifdef __linux__
    if (region.system_v_) {
      const int result = shmdt(static_cast<void*>(region.data_));
      if (result != 0) {
        std::fprintf(stderr,
                     "Axio HugeAlloc: Error freeing SHM buffer for key %d.\n",
                     region.key_);
      }
      continue;
    }
#endif
    delete[] region.data_;
  }
}

RegisteredMemorySlice HugeAlloc::reserve(size_t size) {
  size = round_up_to_multiple(size, kHugepageSize);
  if (size == 0) {
    throw std::overflow_error("Axio HugeAlloc: reservation size overflow");
  }

#ifdef __linux__
  std::ostringstream error_message;
  int shared_memory_key = 0;
  int shared_memory_id = -1;

  while (true) {
    shared_memory_key = static_cast<int>(
        this->random_.next_u64() & static_cast<uint64_t>(INT_MAX));
    shared_memory_id =
        shmget(shared_memory_key, size,
               IPC_CREAT | IPC_EXCL | 0666 | SHM_HUGETLB);
    if (shared_memory_id != -1) break;

    switch (errno) {
      case EEXIST:
        continue;
      case EACCES:
        throw std::runtime_error(
            "Axio HugeAlloc: SHM allocation error. Insufficient permissions.");
      case EINVAL:
        error_message << "Axio HugeAlloc: SHM allocation error: "
                      << "SHMMAX/SHMIN mismatch. size = " << size << " ("
                      << size / AXIO_MB(1) << " MiB).";
        throw std::runtime_error(error_message.str());
      case ENOMEM:
        AXIO_WARN(
            "Axio HugeAlloc: Insufficient hugepages. Can't reserve %zu MiB.\n",
            size / AXIO_MB(1));
        return {};
      default:
        error_message << "Axio HugeAlloc: Unexpected SHM allocation error: "
                      << std::strerror(errno);
        throw std::runtime_error(error_message.str());
    }
  }

  void* attached = shmat(shared_memory_id, nullptr, 0);
  if (attached == reinterpret_cast<void*>(-1)) {
    shmctl(shared_memory_id, IPC_RMID, nullptr);
    throw std::runtime_error("Axio HugeAlloc: shmat() failed for key " +
                             std::to_string(shared_memory_key));
  }
  auto* data = static_cast<uint8_t*>(attached);
  shmctl(shared_memory_id, IPC_RMID, nullptr);

  const unsigned long node_mask =
      1ul << static_cast<unsigned long>(this->numa_node_);
  const long result = mbind(data, size, MPOL_BIND, &node_mask, 32, 0);
  if (result != 0) {
    shmdt(static_cast<void*>(data));
    throw std::runtime_error("Axio HugeAlloc: mbind() failed for key " +
                             std::to_string(shared_memory_key));
  }

  this->owned_regions_.push_back(
      {shared_memory_key, data, size, true});
#else
  auto* data = new (std::nothrow) uint8_t[size];
  if (data == nullptr) return {};
  this->owned_regions_.push_back({-1, data, size, false});
#endif

  this->reserved_bytes_ += size;
  return {data, size, UINT32_MAX};
}

void HugeAlloc::add_registered_region(RegisteredMemorySlice region) {
  assert(region.data_ != nullptr);
  assert(region.size_ != 0);
  assert(region.lkey_ != UINT32_MAX);
  this->registered_regions_.push_back(
      {region.data_, region.size_, region.lkey_, 0});
}

size_t HugeAlloc::_aligned_offset(const RegisteredRegion& region,
                                  size_t alignment) {
  assert(alignment != 0 && (alignment & (alignment - 1)) == 0);
  const uintptr_t base = reinterpret_cast<uintptr_t>(region.data_);
  const uintptr_t current = base + region.next_offset_;
  const uintptr_t aligned =
      (current + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
  if (aligned < current) return std::numeric_limits<size_t>::max();
  return static_cast<size_t>(aligned - base);
}

RegisteredMemorySlice HugeAlloc::allocate(size_t size, size_t alignment) {
  assert(size != 0);
  assert(alignment != 0 && (alignment & (alignment - 1)) == 0);

  for (RegisteredRegion& region : this->registered_regions_) {
    const size_t offset = this->_aligned_offset(region, alignment);
    if (offset == std::numeric_limits<size_t>::max() ||
        offset > region.size_ || size > region.size_ - offset) {
      continue;
    }
    region.next_offset_ = offset + size;
    this->allocated_bytes_ += size;
    return {region.data_ + offset, size, region.lkey_};
  }
  return {};
}

std::vector<RegisteredMemorySlice> HugeAlloc::take_remaining_fixed_slices(
    size_t slice_size) {
  assert(slice_size != 0 && (slice_size & (slice_size - 1)) == 0);
  std::vector<RegisteredMemorySlice> slices;
  for (RegisteredRegion& region : this->registered_regions_) {
    size_t offset = this->_aligned_offset(region, slice_size);
    if (offset == std::numeric_limits<size_t>::max() ||
        offset > region.size_) {
      continue;
    }
    const size_t slice_count = (region.size_ - offset) / slice_size;
    slices.reserve(slices.size() + slice_count);
    for (size_t index = 0; index < slice_count; ++index) {
      slices.push_back(
          {region.data_ + offset + (index * slice_size), slice_size,
           region.lkey_});
    }
    const size_t consumed = slice_count * slice_size;
    region.next_offset_ = offset + consumed;
    this->allocated_bytes_ += consumed;
  }
  return slices;
}

void HugeAlloc::print_statistics() const {
  std::fprintf(stderr, "Axio HugeAlloc statistics:\n");
  std::fprintf(stderr, "Total reserved SHM = %zu bytes (%.2f MiB)\n",
               this->reserved_bytes_,
               1.0 * this->reserved_bytes_ / AXIO_MB(1));
  std::fprintf(stderr, "Initialization slices = %zu bytes (%.2f MiB)\n",
               this->allocated_bytes_,
               1.0 * this->allocated_bytes_ / AXIO_MB(1));
  std::fprintf(stderr, "%zu owned regions, %zu registered regions\n",
               this->owned_regions_.size(), this->registered_regions_.size());
}

}  // namespace axio
