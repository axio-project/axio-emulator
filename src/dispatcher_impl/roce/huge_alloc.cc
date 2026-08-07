#include "huge_alloc.h"

#include <iostream>
#include <utility>

#ifdef __linux__
#include <numaif.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#endif

namespace axio {

class HugeAlloc::ReusableBufferPool {
 public:
  ReusableBufferPool(std::vector<Buffer*> buffers, bool concurrent_access)
      : buffers_(std::move(buffers)), concurrent_access_(concurrent_access) {
    assert(!this->buffers_.empty());
  }

  bool allocate_bulk(Buffer** buffers, size_t count) {
    if (count == 0) return true;
    if (count > this->buffers_.size()) return false;

    size_t start;
    if (this->concurrent_access_) {
      start = this->shared_next_index_.fetch_add(count,
                                                 std::memory_order_relaxed);
    } else {
      start = this->local_next_index_;
      this->local_next_index_ += count;
    }
    size_t allocated_count = 0;
    for (size_t offset = 0;
         offset < this->buffers_.size() && allocated_count < count;
         ++offset) {
      Buffer* buffer =
          this->buffers_[(start + offset) % this->buffers_.size()];
      if (this->_try_acquire(buffer)) {
        buffers[allocated_count++] = buffer;
      }
    }
    if (allocated_count == count) return true;

    while (allocated_count != 0) {
      --allocated_count;
      this->release(buffers[allocated_count]);
      buffers[allocated_count] = nullptr;
    }
    return false;
  }

  void release(Buffer* buffer) {
    if (this->concurrent_access_) {
      buffer->mark_free();
    } else {
      buffer->mark_free_local();
    }
  }

  bool concurrent_access() const { return this->concurrent_access_; }

 private:
  bool _try_acquire(Buffer* buffer) const {
    return this->concurrent_access_ ? buffer->try_acquire()
                                    : buffer->try_acquire_local();
  }

  const std::vector<Buffer*> buffers_;
  const bool concurrent_access_;
  std::atomic<size_t> shared_next_index_{0};
  size_t local_next_index_ = 0;
};

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
  const std::lock_guard<std::mutex> lock(this->mutex_);
  fprintf(stderr, "Axio HugeAlloc statistics:\n");
  fprintf(stderr, "Total reserved SHM = %zu bytes (%.2f MiB)\n",
          this->stats_.shared_memory_reserved_,
          1.0 * this->stats_.shared_memory_reserved_ / AXIO_MB(1));
  fprintf(stderr, "Total memory allocated to user = %zu bytes (%.2f MiB)\n",
          this->stats_.user_allocated_.load(std::memory_order_relaxed),
          1.0 * this->stats_.user_allocated_.load(std::memory_order_relaxed) /
              AXIO_MB(1));

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
  const size_t class_index = this->_class_index(size);
  if (this->reusable_pools_[class_index] != nullptr) {
    ReusableBufferPool* pool = this->reusable_pools_[class_index].get();
    Buffer* buffer = nullptr;
    if (!pool->allocate_bulk(&buffer, 1)) {
      return nullptr;
    }
    const size_t allocated_bytes = max_class_size(class_index);
    if (pool->concurrent_access()) {
      this->stats_.user_allocated_.fetch_add(allocated_bytes,
                                            std::memory_order_relaxed);
    } else {
      this->stats_.user_allocated_.store(
          this->stats_.user_allocated_.load(std::memory_order_relaxed) +
              allocated_bytes,
          std::memory_order_relaxed);
    }
    return buffer;
  }
  const std::lock_guard<std::mutex> lock(this->mutex_);
  return this->_allocate_locked(size);
}

bool HugeAlloc::allocate_bulk(size_t size, Buffer** buffers, size_t count) {
  const size_t class_index = this->_class_index(size);
  if (this->reusable_pools_[class_index] != nullptr) {
    ReusableBufferPool* pool = this->reusable_pools_[class_index].get();
    if (!pool->allocate_bulk(buffers, count)) {
      return false;
    }
    const size_t allocated_bytes = count * max_class_size(class_index);
    if (pool->concurrent_access()) {
      this->stats_.user_allocated_.fetch_add(allocated_bytes,
                                            std::memory_order_relaxed);
    } else {
      this->stats_.user_allocated_.store(
          this->stats_.user_allocated_.load(std::memory_order_relaxed) +
              allocated_bytes,
          std::memory_order_relaxed);
    }
    return true;
  }
  const std::lock_guard<std::mutex> lock(this->mutex_);
  size_t allocated_count = 0;
  for (; allocated_count < count; ++allocated_count) {
    buffers[allocated_count] = this->_allocate_locked(size);
    if (buffers[allocated_count] == nullptr) break;
  }
  if (allocated_count == count) return true;
  while (allocated_count != 0) {
    --allocated_count;
    this->_free_buffer_locked(buffers[allocated_count]);
    buffers[allocated_count] = nullptr;
  }
  return false;
}

void HugeAlloc::prepare_reusable_pool(size_t size, bool concurrent_access) {
  const size_t class_index = this->_class_index(size);
  const std::lock_guard<std::mutex> lock(this->mutex_);
  rt_assert(this->reusable_pools_[class_index] == nullptr,
            "RoCE reusable buffer pool is already prepared");

  for (size_t split_index = kNumClasses - 1;
       split_index > class_index; --split_index) {
    while (!this->free_lists_[split_index].empty()) {
      this->_split_class(split_index);
    }
  }
  rt_assert(!this->free_lists_[class_index].empty(),
            "RoCE reusable buffer pool has no backing buffers");
  this->reusable_pools_[class_index] = std::make_unique<ReusableBufferPool>(
      std::move(this->free_lists_[class_index]), concurrent_access);
}

void HugeAlloc::free_buffer(Buffer* buffer) {
  const size_t class_index = this->_class_index(buffer->class_size_);
  if (this->reusable_pools_[class_index] != nullptr) {
    ReusableBufferPool* pool = this->reusable_pools_[class_index].get();
    assert(buffer->state() != Buffer::kFree);
    pool->release(buffer);
    const size_t previous = pool->concurrent_access()
                                ? this->stats_.user_allocated_.fetch_sub(
                                      buffer->class_size_,
                                      std::memory_order_relaxed)
                                : this->stats_.user_allocated_.load(
                                      std::memory_order_relaxed);
    assert(previous >= buffer->class_size_);
    if (!pool->concurrent_access()) {
      this->stats_.user_allocated_.store(previous - buffer->class_size_,
                                        std::memory_order_relaxed);
    }
    return;
  }
  const std::lock_guard<std::mutex> lock(this->mutex_);
  this->_free_buffer_locked(buffer);
}

void HugeAlloc::free_buffers(Buffer* const* buffers, size_t count) {
  if (count == 0) return;
  const size_t class_index = this->_class_index(buffers[0]->class_size_);
  if (this->reusable_pools_[class_index] != nullptr) {
    ReusableBufferPool* pool = this->reusable_pools_[class_index].get();
    const size_t class_size = max_class_size(class_index);
    for (size_t index = 0; index < count; ++index) {
      assert(buffers[index]->class_size_ == class_size);
      assert(buffers[index]->state() != Buffer::kFree);
      pool->release(buffers[index]);
    }
    const size_t released_bytes = count * class_size;
    const size_t previous = pool->concurrent_access()
                                ? this->stats_.user_allocated_.fetch_sub(
                                      released_bytes,
                                      std::memory_order_relaxed)
                                : this->stats_.user_allocated_.load(
                                      std::memory_order_relaxed);
    assert(previous >= released_bytes);
    if (!pool->concurrent_access()) {
      this->stats_.user_allocated_.store(previous - released_bytes,
                                        std::memory_order_relaxed);
    }
    return;
  }
  const std::lock_guard<std::mutex> lock(this->mutex_);
  for (size_t index = 0; index < count; ++index) {
    this->_free_buffer_locked(buffers[index]);
  }
}

Buffer* HugeAlloc::_allocate_locked(size_t size) {
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

void HugeAlloc::_free_buffer_locked(Buffer* buffer) {
  assert(buffer != nullptr);
  assert(buffer->buf_ != nullptr);
  buffer->mark_free();

  const size_t class_index = this->_class_index(buffer->class_size_);
  assert(max_class_size(class_index) == buffer->class_size_);

  this->free_lists_[class_index].push_back(buffer);
  const size_t previous = this->stats_.user_allocated_.fetch_sub(
      buffer->class_size_, std::memory_order_relaxed);
  assert(previous >= buffer->class_size_);
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
