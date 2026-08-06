#pragma once

#include "common.h"
#include "util/rand.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace axio {

class KeyValueStore {
 public:
  static constexpr size_t kKeySize = 16;
  static constexpr size_t kValueSize = 64;

  struct Key {
    uint8_t bytes_[kKeySize];
  };

  struct Value {
    uint8_t bytes_[kValueSize];
  };

  explicit KeyValueStore(size_t initial_size) : max_key_(initial_size) {
    for (size_t i = 0; i < initial_size; i++) {
      Key key;
      size_t encoded_key = i;
      for (size_t byte_index = 0; byte_index < kKeySize; byte_index++) {
        key.bytes_[byte_index] = encoded_key & ((1 << 8) - 1);
        encoded_key >>= 8;
      }

      Value value;
      size_t encoded_value = i * 0x12345 + 0x10501;
      for (size_t byte_index = 0; byte_index < kValueSize; byte_index++) {
        value.bytes_[byte_index] = encoded_value & ((1 << 8) - 1);
        encoded_value >>= 8;
      }
      this->put(key, value);
    }
  }

  void put(const Key key, const Value value) { this->entries_[key] = value; }

  void put_test(const Key key, const Value value) {
    AXIO_UNUSED(key);
    uint32_t random_key = this->random_.next_u32() % this->max_key_;
    this->entries_[*reinterpret_cast<Key*>(&random_key)] = value;
  }

  std::optional<Value> get(const Key key) {
    auto entry = this->entries_.find(key);
    if (entry != this->entries_.end()) {
      return entry->second;
    }
    return std::nullopt;
  }

 private:
  struct Hash {
    size_t operator()(const Key& key) const {
      size_t hash = 0;
      for (size_t i = 0; i < kKeySize; i++) {
        hash = hash * 271 + key.bytes_[i];
      }
      return hash;
    }
  };

  struct Equal {
    bool operator()(const Key& first, const Key& second) const {
      for (size_t i = 0; i < kKeySize; i++) {
        if (first.bytes_[i] != second.bytes_[i]) {
          return false;
        }
      }
      return true;
    }
  };

  std::unordered_map<Key, Value, Hash, Equal> entries_;
  size_t max_key_ = 0;
  FastRandom random_;
};

}  // namespace axio
