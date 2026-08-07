#include "dispatcher_impl/roce/huge_alloc.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr size_t kWorkerCount = 6;
constexpr size_t kBatchSize = 16;
constexpr size_t kIterationCount = 2000;
constexpr size_t kBufferSize = 4096;

bool exercise_shared_allocator() {
  auto storage = std::make_unique<uint8_t[]>(axio::HugeAlloc::kMaxClassSize);
  axio::HugeAlloc allocator(axio::HugeAlloc::kMaxClassSize, 0);
  allocator.add_raw_buffer(
      axio::Buffer(storage.get(), axio::HugeAlloc::kMaxClassSize, 1),
      axio::HugeAlloc::kMaxClassSize);

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
        if (!allocator.allocate_bulk(kBufferSize, buffers.data(),
                                     buffers.size())) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        allocator.free_buffers(buffers.data(), buffers.size());
      }
    });
  }
  while (ready_count.load(std::memory_order_acquire) != kWorkerCount) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (std::thread& worker : workers) worker.join();
  return !failed.load(std::memory_order_relaxed) &&
         allocator.user_allocated_bytes() == 0;
}

}  // namespace

int main() {
  if (!exercise_shared_allocator()) {
    std::fprintf(stderr, "shared HugeAlloc concurrency test failed\n");
    return 1;
  }
  std::puts("Axio shared HugeAlloc concurrency test passed");
  return 0;
}
