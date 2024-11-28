#pragma once
#include "util/rand.h"
#include <unordered_map>
#include <optional>

namespace dperf {
class KV {
public:
    static constexpr size_t kKeySize = 16;
    static constexpr size_t kValueSize = 64;

    typedef struct {
        uint8_t key[kKeySize];
    } key_t;
    
    typedef struct {
        uint8_t value[kValueSize];
    } value_t;

    KV(size_t initial_size) {
        max_key = initial_size;
        for(size_t i = 0; i < initial_size; i++){
        key_t key;
        size_t k = i;
        for(size_t t = 0; t < kKeySize; t++) {
            key.key[t] = k & ((1<<8) - 1);
            k >>= 8;
        }
        value_t value;
        size_t v = i*0x12345+0x10501;
        for(size_t t = 0; t < kValueSize; t++) {
            value.value[t] = v & ((1<<8) - 1);
            v >>= 8;
        }
        // printf("put:[%d,%d]\n", key.key_[0], value.value_[0]);
        put(key, value);
        }
    }

    ~KV() {}

    void put(const key_t key, const value_t value) {
        kvmap[key] = value;
    }

    void put_test(const key_t key, const value_t value) {
        uint32_t key_32 = rand_.next_u32() % max_key;
        kvmap[*(reinterpret_cast<key_t*>(&key_32))] = value;
    }

    std::optional<value_t> get(const key_t key) {
        auto it = kvmap.find(key);
        if (it != kvmap.end()) {
            return it->second;
        }
        return std::nullopt;
    }

private:
    struct HashFunc {
        std::size_t operator()(const key_t &key) const {
            std::size_t hash = 0;
            for (size_t i = 0; i < kKeySize; i++) {
                hash = hash * 271 + key.key[i];
            }
            return hash;
        }
    };
    struct CompareFunc {
        bool operator()(const key_t &key1, const key_t &key2) const {
            for(size_t i = 0; i < kKeySize; i++){
                if(key1.key[i] != key2.key[i]) return false;
            }
            return true;
        }
    };
    std::unordered_map<key_t, value_t, HashFunc, CompareFunc> kvmap;
    /// DEBUG
    size_t max_key = 0;
    FastRand rand_;
};
}