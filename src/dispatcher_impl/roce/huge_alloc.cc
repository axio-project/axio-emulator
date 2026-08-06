#include "huge_alloc.h"

#include <iostream>

#ifdef __linux__
#include <numaif.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

namespace axio {

HugeAlloc::HugeAlloc(size_t initial_size, size_t numa_node)
    : numa_node_(numa_node) {
  assert(numa_node <= kMaxNumaNodes);

  if (initial_size < kMaxClassSize) {
    initial_size = kMaxClassSize;
  }
  this->previous_allocation_size_ = initial_size;
}

HugeAlloc::~HugeAlloc() {
  for (SharedMemoryRegion& region : this->shared_memory_regions_) {
#ifdef __linux__
    const int result =
        shmdt(static_cast<void*>(const_cast<uint8_t*>(region.buffer_)));
    if (result != 0) {
      fprintf(stderr, "Axio HugeAlloc: Error freeing SHM buffer for key %d.\n",
              region.key_);
      exit(-1);
    }
#else
    rt_assert(false, "HugeAlloc is not implemented on Windows");
#endif
  }
}

void HugeAlloc::print_statistics() {
  fprintf(stderr, "Axio HugeAlloc statistics:\n");
  fprintf(stderr, "Total reserved SHM = %zu bytes (%.2f MiB)\n",
          this->stats_.shared_memory_reserved_,
          1.0 * this->stats_.shared_memory_reserved_ / AXIO_MB(1));
  fprintf(stderr, "Total memory allocated to user = %zu bytes (%.2f MiB)\n",
          this->stats_.user_allocated_,
          1.0 * this->stats_.user_allocated_ / AXIO_MB(1));

  fprintf(stderr, "%zu SHM regions\n", this->shared_memory_regions_.size());
  size_t region_index = 0;
  for (SharedMemoryRegion& region : this->shared_memory_regions_) {
    fprintf(stderr, "Region %zu, size %zu MiB\n", region_index,
            region.size_ / AXIO_MB(1));
    region_index++;
  }

  fprintf(stderr, "Size classes:\n");
  for (size_t i = 0; i < kNumClasses; i++) {
    size_t class_size = max_class_size(i);
    if (class_size < AXIO_KB(1)) {
      fprintf(stderr, "\t%zu B: %zu buffers\n", class_size,
              this->free_lists_[i].size());
    } else if (class_size < AXIO_MB(1)) {
      fprintf(stderr, "\t%zu KiB: %zu buffers\n",
              class_size / AXIO_KB(1), this->free_lists_[i].size());
    } else {
      fprintf(stderr, "\t%zu MiB: %zu buffers\n",
              class_size / AXIO_MB(1), this->free_lists_[i].size());
    }
  }
}

Buffer HugeAlloc::allocate_raw(size_t size,
                               MemoryRegistration registration) {
#ifdef __linux__
  std::ostringstream error_message;
  size = round_up<kHugepageSize>(size);
  int shared_memory_key;
  int shared_memory_id;

  while (true) {
    shared_memory_key = static_cast<int>(this->random_.next_u64());
    shared_memory_key = std::abs(shared_memory_key);

    shared_memory_id =
        shmget(shared_memory_key, size,
               IPC_CREAT | IPC_EXCL | 0666 | SHM_HUGETLB);

    if (shared_memory_id == -1) {
      switch (errno) {
        case EEXIST:
          continue;
        case EACCES:
          error_message << "Axio HugeAlloc: SHM allocation error. "
                        << "Insufficient permissions.";
          throw std::runtime_error(error_message.str());
        case EINVAL:
          error_message << "Axio HugeAlloc: SHM allocation error: "
                        << "SHMMAX/SHMIN mismatch. size = "
                        << std::to_string(size) << " ("
                        << std::to_string(size / AXIO_MB(1)) << " MiB).";
          throw std::runtime_error(error_message.str());
        case ENOMEM:
          AXIO_WARN(
              "Axio HugeAlloc: Insufficient hugepages. Can't reserve %zu MiB.\n",
              size / AXIO_MB(1));
          return Buffer(nullptr, 0, 0);
        default:
          error_message << "Axio HugeAlloc: Unexpected SHM allocation error: "
                        << strerror(errno);
          throw std::runtime_error(error_message.str());
      }
    }
    break;
  }

  auto* shared_memory_buffer =
      static_cast<uint8_t*>(shmat(shared_memory_id, nullptr, 0));
  rt_assert(shared_memory_buffer != nullptr,
            "Axio HugeAlloc: shmat() failed. Key = " +
                std::to_string(shared_memory_key));

  shmctl(shared_memory_id, IPC_RMID, nullptr);

  const unsigned long node_mask =
      1ul << static_cast<unsigned long>(this->numa_node_);
  long result = mbind(shared_memory_buffer, size, MPOL_BIND, &node_mask, 32, 0);
  rt_assert(result == 0,
            "Axio HugeAlloc: mbind() failed. Key " +
                std::to_string(shared_memory_key));

  bool registration_enabled =
      registration == MemoryRegistration::kEnabled;
  this->shared_memory_regions_.push_back(SharedMemoryRegion(
      shared_memory_key, shared_memory_buffer, size, registration_enabled));
  this->stats_.shared_memory_reserved_ += size;

  return Buffer(shared_memory_buffer, SIZE_MAX, UINT32_MAX);
#else
  uint8_t* buffer = new uint8_t[size];
  return Buffer(buffer, SIZE_MAX, UINT32_MAX);
#endif
}

Buffer* HugeAlloc::allocate(size_t size) {
  assert(size <= kMaxClassSize);

  size_t class_index = this->_class_index(size);
  assert(class_index < kNumClasses);

  if (!this->free_lists_[class_index].empty()) {
    return this->_allocate_from_class(class_index);
  }

  size_t next_class_index = class_index + 1;
  for (; next_class_index < kNumClasses; next_class_index++) {
    if (!this->free_lists_[next_class_index].empty()) {
      break;
    }
  }

  if (next_class_index == kNumClasses) {
    // Growing the region dynamically is intentionally disabled. The caller
    // receives no buffer when the pre-registered pool is exhausted.
    return nullptr;
  }

  assert(next_class_index < kNumClasses);
  while (next_class_index != class_index) {
    this->_split_class(next_class_index);
    next_class_index--;
  }

  assert(!this->free_lists_[class_index].empty());
  return this->_allocate_from_class(class_index);
}

bool HugeAlloc::_reserve_hugepages(size_t size) {
  assert(size >= kMaxClassSize);
  Buffer buffer =
      this->allocate_raw(size, MemoryRegistration::kEnabled);
  if (buffer.buf_ == nullptr) {
    return false;
  }

  size_t buffer_count = size / kMaxClassSize;
  assert(buffer_count >= 1);
  for (size_t i = 0; i < buffer_count; i++) {
    uint8_t* data = buffer.buf_ + (i * kMaxClassSize);
    uint32_t local_key = buffer.lkey_;
    Buffer* split_buffer = new Buffer(data, kMaxClassSize, local_key);
    assert(split_buffer != nullptr);
    this->free_lists_[kNumClasses - 1].push_back(split_buffer);
  }

  return true;
}

void HugeAlloc::add_raw_buffer(Buffer buffer, size_t size) {
  if (size >= kMaxClassSize) {
    size_t buffer_count = size / kMaxClassSize;
    assert(buffer_count >= 1);
    for (size_t i = 0; i < buffer_count; i++) {
      uint8_t* data = buffer.buf_ + (i * kMaxClassSize);
      uint32_t local_key = buffer.lkey_;
      Buffer* split_buffer = new Buffer(data, kMaxClassSize, local_key);
      assert(split_buffer != nullptr);
      this->free_lists_[kNumClasses - 1].push_back(split_buffer);
    }
    size_t remaining_size = size % kMaxClassSize;
    if (remaining_size > 0) {
      this->add_raw_buffer(buffer, remaining_size);
    }
  } else {
    size_t class_index = this->_class_index(size);
    assert(class_index < kNumClasses);
    Buffer* split_buffer =
        new Buffer(buffer.buf_, class_index, buffer.lkey_);
    assert(split_buffer != nullptr);
    this->free_lists_[class_index].push_back(split_buffer);
  }
}

}  // namespace axio
