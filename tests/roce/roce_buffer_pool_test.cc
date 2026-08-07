#include "dispatcher_impl/roce/roce_buffer_pool.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr size_t kBufferSize = 4096;
constexpr size_t kLocalCapacity = 64;
constexpr size_t kSharedCapacity = 256;
constexpr size_t kWorkerCount = 6;
constexpr size_t kBatchSize = 16;
constexpr size_t kIterationCount = 2000;
constexpr uint32_t kLocalKey = 23;

using AlignedStorage = std::unique_ptr<uint8_t, decltype(&std::free)>;

AlignedStorage make_storage(size_t size) {
  return AlignedStorage(
      static_cast<uint8_t*>(std::aligned_alloc(kBufferSize, size)),
      &std::free);
}

bool exercise_local_pool() {
  AlignedStorage storage = make_storage(kLocalCapacity * kBufferSize);
  if (storage == nullptr) return false;
  axio::HugeAlloc allocator(0);
  allocator.add_registered_region(
      {storage.get(), kLocalCapacity * kBufferSize, kLocalKey});
  axio::RoceBufferPool pool(&allocator, kBufferSize,
                            axio::RoceBufferPoolAccess::kLocal);

  if (pool.capacity() != kLocalCapacity || pool.allocated_count() != 0) {
    return false;
  }
  axio::Buffer external_buffer(storage.get(), kBufferSize, kLocalKey);
  if (pool.owns(nullptr) || pool.owns(&external_buffer)) return false;

  std::vector<axio::Buffer*> all_buffers(kLocalCapacity);
  if (!pool.allocate_bulk(all_buffers.data(), all_buffers.size())) return false;
  for (axio::Buffer* buffer : all_buffers) {
    if (buffer == nullptr || !pool.owns(buffer)) return false;
  }
  if (pool.allocate() != nullptr ||
      pool.allocated_count() != kLocalCapacity) {
    return false;
  }

  axio::Buffer* expected_reuse = all_buffers.back();
  pool.free(expected_reuse);
  if (pool.allocate() != expected_reuse) return false;

  pool.free_bulk(all_buffers.data(), all_buffers.size());
  if (pool.allocated_count() != 0) return false;

  std::vector<axio::Buffer*> held(kLocalCapacity - 2);
  if (!pool.allocate_bulk(held.data(), held.size())) return false;
  std::array<axio::Buffer*, 3> failed_batch = {
      reinterpret_cast<axio::Buffer*>(1),
      reinterpret_cast<axio::Buffer*>(1),
      reinterpret_cast<axio::Buffer*>(1)};
  if (pool.allocate_bulk(failed_batch.data(), failed_batch.size())) {
    return false;
  }
  for (axio::Buffer* buffer : failed_batch) {
    if (buffer != nullptr) return false;
  }
  if (pool.allocated_count() != held.size()) return false;
  pool.free_bulk(held.data(), held.size());

  return pool.allocate_bulk(nullptr, 0) && pool.allocated_count() == 0;
}

bool exercise_shared_pool() {
  AlignedStorage storage = make_storage(kSharedCapacity * kBufferSize);
  if (storage == nullptr) return false;
  axio::HugeAlloc allocator(0);
  allocator.add_registered_region(
      {storage.get(), kSharedCapacity * kBufferSize, kLocalKey});
  axio::RoceBufferPool pool(&allocator, kBufferSize,
                            axio::RoceBufferPoolAccess::kShared);
  if (pool.capacity() != kSharedCapacity) {
    std::fprintf(stderr,
                 "shared capacity mismatch: expected %zu, got %zu, "
                 "base alignment %zu\n",
                 kSharedCapacity, pool.capacity(),
                 reinterpret_cast<uintptr_t>(storage.get()) % kBufferSize);
    return false;
  }

  auto active = std::make_unique<std::atomic<uint8_t>[]>(kSharedCapacity);
  for (size_t index = 0; index < kSharedCapacity; ++index) {
    active[index].store(0, std::memory_order_relaxed);
  }

  std::atomic<size_t> ready_count{0};
  std::atomic<bool> start{false};
  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  workers.reserve(kWorkerCount);
  for (size_t worker = 0; worker < kWorkerCount; ++worker) {
    workers.emplace_back([&]() {
      ready_count.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (size_t iteration = 0; iteration < kIterationCount; ++iteration) {
        std::array<axio::Buffer*, kBatchSize> buffers{};
        if (!pool.allocate_bulk(buffers.data(), buffers.size())) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        for (axio::Buffer* buffer : buffers) {
          const size_t index =
              static_cast<size_t>(buffer->data() - storage.get()) /
              kBufferSize;
          if (index >= kSharedCapacity ||
              active[index].exchange(1, std::memory_order_acq_rel) != 0) {
            failed.store(true, std::memory_order_relaxed);
          }
        }
        for (axio::Buffer* buffer : buffers) {
          const size_t index =
              static_cast<size_t>(buffer->data() - storage.get()) /
              kBufferSize;
          if (active[index].exchange(0, std::memory_order_acq_rel) != 1) {
            failed.store(true, std::memory_order_relaxed);
          }
        }
        pool.free_bulk(buffers.data(), buffers.size());
      }
    });
  }

  while (ready_count.load(std::memory_order_acquire) != kWorkerCount) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (std::thread& worker : workers) worker.join();

  if (failed.load(std::memory_order_relaxed) ||
      pool.allocated_count() != 0) {
    return false;
  }
  std::vector<axio::Buffer*> all_buffers(kSharedCapacity);
  if (!pool.allocate_bulk(all_buffers.data(), all_buffers.size())) return false;
  pool.free_bulk(all_buffers.data(), all_buffers.size());
  return pool.allocated_count() == 0;
}

}  // namespace

int main() {
  if (!exercise_local_pool()) {
    std::fprintf(stderr, "local RoceBufferPool test failed\n");
    return 1;
  }
  if (!exercise_shared_pool()) {
    std::fprintf(stderr, "shared RoceBufferPool test failed\n");
    return 1;
  }
  std::puts("RoceBufferPool test passed");
  return 0;
}
