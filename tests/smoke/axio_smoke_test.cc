#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>

#include "common.h"
#include "util/barrier.h"
#include "util/lock_free_queue.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

bool test_common_constants() {
  static_assert(dperf::kMaxPhyPorts > 0);
  static_assert(dperf::kWorkspaceMaxNum > 0);
  static_assert(dperf::kWsQueueSize > 1);
  static_assert((dperf::kWsQueueSize & (dperf::kWsQueueSize - 1)) == 0);
  static_assert(dperf::kInvaildWorkspaceType == (uint8_t{1} << dperf::kWorkspaceTypeNum));
  return true;
}

bool test_lock_free_queue_lifecycle() {
  dperf::lock_free_queue queue;
  uint8_t packet = 0;

  for (size_t i = 0; i < dperf::kWsQueueSize - 1; ++i) {
    if (!expect(queue.enqueue(&packet), "queue rejected an entry before becoming full")) {
      return false;
    }
  }
  if (!expect(!queue.enqueue(&packet), "queue accepted an entry after becoming full") ||
      !expect(queue.get_size() == dperf::kWsQueueSize - 1, "queue reported the wrong full size")) {
    return false;
  }

  for (size_t i = 0; i < dperf::kWsQueueSize - 1; ++i) {
    if (!expect(queue.dequeue() == &packet, "queue did not preserve an enqueued pointer")) {
      return false;
    }
  }
  if (!expect(queue.dequeue() == nullptr, "queue did not report empty") ||
      !expect(queue.get_size() == 0, "queue reported a non-zero empty size")) {
    return false;
  }

  return expect(queue.enqueue(&packet), "queue could not enqueue after index wrap") &&
         expect(queue.dequeue() == &packet, "queue could not dequeue after index wrap");
}

bool test_thread_barrier_lifecycle() {
  dperf::ThreadBarrier barrier(2);
  std::atomic<int> arrivals{0};
  std::atomic<int> departures{0};

  auto worker = [&]() {
    arrivals.fetch_add(1, std::memory_order_relaxed);
    barrier.wait();
    if (arrivals.load(std::memory_order_relaxed) == 2) {
      departures.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::thread first(worker);
  std::thread second(worker);
  first.join();
  second.join();

  return expect(departures.load(std::memory_order_relaxed) == 2, "threads did not leave the barrier together");
}

}  // namespace

int main() {
  if (!test_common_constants() || !test_lock_free_queue_lifecycle() || !test_thread_barrier_lifecycle()) {
    return 1;
  }

  std::cout << "axio smoke test passed\n";
  return 0;
}
