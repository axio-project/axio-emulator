#pragma once

#include "common.h"
#include "util/rand.h"

#include <cstddef>
#include <cstdint>
#include <algorithm>
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

  explicit KeyValueStore(size_t initial_size, size_t first_key = 0,
                         uint64_t random_seed = 1)
      : first_key_(first_key), max_key_(initial_size), random_(random_seed) {
    for (size_t i = 0; i < initial_size; i++) {
      const Key key = this->key_for_local_index(i);

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
    if (this->max_key_ == 0) return;
    const size_t local_index = this->random_.next_u64() % this->max_key_;
    this->entries_[this->key_for_local_index(local_index)] = value;
  }

  std::optional<Value> get(const Key key) {
    auto entry = this->entries_.find(key);
    if (entry != this->entries_.end()) {
      return entry->second;
    }
    return std::nullopt;
  }

  Key key_for_local_index(size_t local_index) const {
    Key key{};
    size_t encoded_key = this->first_key_ + local_index;
    for (size_t byte_index = 0; byte_index < kKeySize; ++byte_index) {
      key.bytes_[byte_index] = static_cast<uint8_t>(encoded_key & 0xffU);
      encoded_key >>= 8;
    }
    return key;
  }

  Key resolve_local_key(const Key& requested) const {
    if (this->max_key_ == 0) return Key{};
    size_t encoded = 0;
    const size_t encoded_bytes = std::min(kKeySize, sizeof(size_t));
    for (size_t byte_index = 0; byte_index < encoded_bytes; ++byte_index) {
      encoded |= static_cast<size_t>(requested.bytes_[byte_index])
                 << (byte_index * 8U);
    }
    return this->key_for_local_index(encoded % this->max_key_);
  }

  size_t size() const { return this->entries_.size(); }

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
  size_t first_key_ = 0;
  size_t max_key_ = 0;
  FastRandom random_;
};

}  // namespace axio
