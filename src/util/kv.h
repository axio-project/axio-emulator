#pragma once
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
        for(size_t i = 0; i < initial_size; i++){
            key_t key;
            *(size_t*)key.key = i;
            value_t value;
            *(size_t*)value.value = i*0x12345+0x010501;
            put(key, value);
        }
    }

    ~KV() {}

    void put(const key_t key, const value_t value) {
        kvmap[key] = value;
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

};
}