#include "axio/workloads/workload_semantics.h"
#include "util/kv.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void test_memory_workload() {
  constexpr size_t kStateBytes = 4 * 1024 * 1024;
  constexpr size_t kAccessBytes = 1024;
  std::vector<uint8_t> first(kStateBytes, 0x5a);
  std::vector<uint8_t> second(kStateBytes, 0x5a);

  axio::workloads::MemoryWorkload first_workload(kStateBytes, kAccessBytes, 7);
  axio::workloads::MemoryWorkload second_workload(kStateBytes, kAccessBytes, 7);
  const size_t first_offset = first_workload.read_modify_write(first.data());
  const size_t second_offset = second_workload.read_modify_write(second.data());

  expect(first_offset == second_offset,
         "M-App offset sequence must be deterministic for a fixed seed");
  expect(first_offset % kAccessBytes == 0,
         "M-App accesses must select an aligned region");
  expect(first == second,
         "M-App read-modify-write data must be deterministic");
  const size_t changed = static_cast<size_t>(std::count_if(
      first.begin(), first.end(), [](uint8_t value) { return value != 0x5a; }));
  expect(changed == kAccessBytes,
         "one M-App request must modify exactly access_bytes_per_message");
}

void test_key_value_sharding() {
  constexpr size_t kEntries = 16384;
  constexpr size_t kShards = 6;
  size_t total = 0;
  size_t smallest = kEntries;
  size_t largest = 0;
  for (size_t shard = 0; shard < kShards; ++shard) {
    const auto range =
        axio::workloads::key_value_shard(kEntries, kShards, shard);
    total += range.size;
    smallest = std::min(smallest, range.size);
    largest = std::max(largest, range.size);
  }
  expect(total == kEntries, "key-value shards must preserve the entry count");
  expect(largest - smallest <= 1,
         "key-value entries must be distributed evenly across workspaces");
}

void test_key_value_operation_mix() {
  axio::workloads::DeterministicOperationMix mix(0.5);
  size_t gets = 0;
  size_t puts = 0;
  for (size_t index = 0; index < 100; ++index) {
    if (mix.next() == axio::workloads::KeyValueOperation::kGet) {
      ++gets;
    } else {
      ++puts;
    }
  }
  expect(gets == 50 && puts == 50,
         "get_ratio=0.5 must produce a deterministic 1:1 mix");
}

void test_key_value_store() {
  axio::KeyValueStore store(32, 4, 11);
  expect(store.size() == 32, "key-value store must initialize its shard");

  const axio::KeyValueStore::Key key = store.key_for_local_index(3);
  auto value = store.get(key);
  expect(value.has_value(), "initialized shard keys must be readable");
  value->bytes_[0] = 0x7f;
  store.put(key, *value);
  expect(store.get(key)->bytes_[0] == 0x7f,
         "PUT must update the addressed local key");
}

}  // namespace

int main() {
  test_memory_workload();
  test_key_value_sharding();
  test_key_value_operation_mix();
  test_key_value_store();
  return 0;
}
