#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include "common.h"
#include "config.h"
#include "util/barrier.h"
#include "util/lock_free_queue.h"
#include "util/ring_buffer.h"
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
  static_assert(axio::kInvalidWorkspaceType == (uint8_t{1} << axio::kWorkspaceTypeNum));
  return true;
}

bool test_config_loading(const std::string& repository_root) {
  axio::UserConfig config(repository_root + "/config/send_config");

  return expect(config.workloads().size() == 4, "config loaded the wrong workload count") &&
         expect(config.numa_node() == 0, "config loaded the wrong NUMA node") &&
         expect(config.physical_port() == 0, "config loaded the wrong physical port") &&
         expect(config.iteration_count() == 30, "config loaded the wrong iteration count") &&
         expect(config.duration_seconds() == 1, "config loaded the wrong duration") &&
         expect(config.tunables().app_core_count_ == 4,
                "config loaded the wrong application core count") &&
         expect(config.tunables().nic_rx_post_size_ == 32,
                "config loaded the wrong NIC RX post size");
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

bool test_ring_buffer_lifecycle() {
  uint8_t storage[8] = {0};
  uint64_t input_index = 0;
  uint64_t output_index = 0;
  axio::RingBuffer ring(storage, sizeof(storage), sizeof(uint8_t),
                        &input_index, &output_index);

  const uint8_t first_input[] = {1, 2, 3, 4, 5};
  uint8_t first_output[sizeof(first_input)] = {0};
  if (!expect(ring.copy_in(first_input, sizeof(first_input)) == 5,
              "ring buffer rejected an available write") ||
      !expect(ring.used_length() == 5,
              "ring buffer reported the wrong used length") ||
      !expect(ring.copy_out(first_output, sizeof(first_output)) == 5,
              "ring buffer rejected an available read") ||
      !expect(std::memcmp(first_input, first_output, sizeof(first_input)) == 0,
              "ring buffer changed copied bytes")) {
    return false;
  }

  const uint8_t wrapped_input[] = {6, 7, 8, 9, 10, 11};
  uint8_t wrapped_output[sizeof(wrapped_input)] = {0};
  return expect(ring.copy_in(wrapped_input, sizeof(wrapped_input)) == 6,
                "ring buffer rejected a wrapped write") &&
         expect(ring.copy_out(wrapped_output, sizeof(wrapped_output)) == 6,
                "ring buffer rejected a wrapped read") &&
         expect(std::memcmp(wrapped_input, wrapped_output,
                            sizeof(wrapped_input)) == 0,
                "ring buffer changed wrapped bytes") &&
         expect(ring.empty(), "ring buffer did not return to empty");
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

int main(int argc, char** argv) {
  const std::string repository_root = argc > 1 ? argv[1] : ".";
  if (!test_common_constants() || !test_config_loading(repository_root) ||
      !test_lock_free_queue_lifecycle() ||
      !test_rule_table_lifecycle() || !test_ring_buffer_lifecycle() ||
      !test_thread_barrier_lifecycle()) {
    return 1;
  }

  std::cout << "axio smoke test passed\n";
  return 0;
}
