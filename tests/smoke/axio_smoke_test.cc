#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>

#include "common.h"
#include "util/barrier.h"
#include "util/lock_free_queue.h"
#include "util/rule_table.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

bool test_common_constants() {
  static_assert(axio::kMaxPhyPorts > 0);
  static_assert(axio::kWorkspaceMaxNum > 0);
  static_assert(axio::kWsQueueSize > 1);
  static_assert((axio::kWsQueueSize & (axio::kWsQueueSize - 1)) == 0);
  static_assert(axio::kInvaildWorkspaceType == (uint8_t{1} << axio::kWorkspaceTypeNum));
  return true;
}

bool test_lock_free_queue_lifecycle() {
  axio::LockFreeQueue queue;
  uint8_t packet = 0;

  for (size_t i = 0; i < axio::kWsQueueSize - 1; ++i) {
    if (!expect(queue.enqueue(&packet), "queue rejected an entry before becoming full")) {
      return false;
    }
  }
  if (!expect(!queue.enqueue(&packet), "queue accepted an entry after becoming full") ||
      !expect(queue.size() == axio::kWsQueueSize - 1, "queue reported the wrong full size")) {
    return false;
  }

  for (size_t i = 0; i < axio::kWsQueueSize - 1; ++i) {
    if (!expect(queue.dequeue() == &packet, "queue did not preserve an enqueued pointer")) {
      return false;
    }
  }
  if (!expect(queue.dequeue() == nullptr, "queue did not report empty") ||
      !expect(queue.size() == 0, "queue reported a non-zero empty size")) {
    return false;
  }

  return expect(queue.enqueue(&packet), "queue could not enqueue after index wrap") &&
         expect(queue.dequeue() == &packet, "queue could not dequeue after index wrap");
}

bool test_rule_table_lifecycle() {
  axio::RuleTable routes;
  routes.add_route(7, 3);
  routes.add_route(7, 5);

  if (!expect(routes.select_next(7) == 3, "route table did not select the first workspace") ||
      !expect(routes.select_next(7) == 5, "route table did not select the second workspace") ||
      !expect(routes.select_next(7) == 3, "route table did not wrap its round-robin index")) {
    return false;
  }

  if (!expect(routes.try_acquire_inflight_budget(7, axio::kInflightMessageBudget),
              "route table rejected its available inflight budget") ||
      !expect(!routes.try_acquire_inflight_budget(7, 1),
              "route table overcommitted its inflight budget")) {
    return false;
  }

  routes.release_inflight_budget(7, 4);
  if (!expect(routes.inflight_budget(7) == 4, "route table returned the wrong inflight budget")) {
    return false;
  }

  routes.remove_route(7, 3);
  const auto workspace_ids = routes.workspace_ids(7);
  return expect(workspace_ids.size() == 1 && workspace_ids.front() == 5,
                "route table removed the wrong workspace");
}

bool test_thread_barrier_lifecycle() {
  axio::ThreadBarrier barrier(2);
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
  if (!test_common_constants() || !test_lock_free_queue_lifecycle() ||
      !test_rule_table_lifecycle() || !test_thread_barrier_lifecycle()) {
    return 1;
  }

  std::cout << "axio smoke test passed\n";
  return 0;
}
