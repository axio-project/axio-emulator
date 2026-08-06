#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>

#include "common.h"
#include "config.h"
#include "util/barrier.h"
#include "util/lock_free_queue.h"
#include "util/mgnt_connection.h"
#include "util/qpinfo.hh"
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

bool test_queue_pair_info_round_trip() {
  static_assert(std::is_standard_layout_v<axio::QueuePairInfo>);

  uint8_t gid[16];
  uint8_t mac_address[6];
  for (size_t i = 0; i < sizeof(gid); i++) {
    gid[i] = static_cast<uint8_t>(i);
  }
  for (size_t i = 0; i < sizeof(mac_address); i++) {
    mac_address[i] = static_cast<uint8_t>(16 + i);
  }

  axio::QueuePairInfo original(7, 3, gid, 2048, "host-a", "mlx5_0");
  original.gid_table_index_ = 4;
  original.set_mac(mac_address);
  original.initialized_ = true;

  const std::string serialized = original.serialize();
  const std::string expected_serialized =
      "qp_num:7;lid:3;gid:0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,;"
      "gid_table_index:4;mac:16,17,18,19,20,21,;mtu:2048;"
      "hostname:host-a;nic_name:mlx5_0;is_initialized:1";
  axio::QueuePairInfo restored;
  restored.deserialize(serialized);
  return expect(serialized == expected_serialized,
                "queue-pair wire format changed") &&
         expect(restored.queue_pair_number_ == original.queue_pair_number_,
                "queue-pair number changed during serialization") &&
         expect(restored.lid_ == original.lid_,
                "queue-pair LID changed during serialization") &&
         expect(std::memcmp(restored.gid_, original.gid_, sizeof(gid)) == 0,
                "queue-pair GID changed during serialization") &&
         expect(restored.gid_table_index_ == original.gid_table_index_,
                "queue-pair GID index changed during serialization") &&
         expect(std::memcmp(restored.mac_address_, original.mac_address_,
                            sizeof(mac_address)) == 0,
                "queue-pair MAC changed during serialization") &&
         expect(restored.mtu_ == original.mtu_,
                "queue-pair MTU changed during serialization") &&
         expect(std::strcmp(restored.hostname_, original.hostname_) == 0,
                "queue-pair hostname changed during serialization") &&
         expect(std::strcmp(restored.nic_name_, original.nic_name_) == 0,
                "queue-pair NIC name changed during serialization") &&
         expect(restored.initialized_ == original.initialized_,
                "queue-pair status changed during serialization");
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
      !test_queue_pair_info_round_trip() ||
      !test_thread_barrier_lifecycle()) {
    return 1;
  }

  std::cout << "axio smoke test passed\n";
  return 0;
}
