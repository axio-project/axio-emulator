/**
 * @file backend_capabilities.cc
 * @brief Canonical capability matrix for Axio dispatcher backends.
 */
#include "axio/config/backend_capabilities.h"

#include <algorithm>
#include <stdexcept>

namespace axio::config {
namespace {

bool is_power_of_two(uint32_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

const BackendCapabilities kDpdkCapabilities = {
    Backend::kDpdk,
    {
        MempoolHandler::kRingMpMc,
        MempoolHandler::kRingSpSc,
        MempoolHandler::kRingMpSc,
        MempoolHandler::kRingSpMc,
        MempoolHandler::kRingMtRts,
        MempoolHandler::kRingMtHts,
        MempoolHandler::kStack,
        MempoolHandler::kLfStack,
        MempoolHandler::kBucket,
    },
    {PacketHandler::kEmpty, PacketHandler::kEcho},
    {},
    64,
    32768,
    true,
    0,
    512,
    1,
};

const BackendCapabilities kRoceCapabilities = {
    Backend::kRoce,
    {MempoolHandler::kHugeAlloc},
    {PacketHandler::kEmpty},
    {1024, 2048, 4096},
    1024,
    4096,
    true,
    2048,
    0,
    0,
};

template <typename T>
bool contains(const std::vector<T>& values, T value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

bool BackendCapabilities::supports_mempool_handler(
    MempoolHandler handler) const {
  return contains(this->mempool_handlers, handler);
}

bool BackendCapabilities::supports_packet_handler(PacketHandler handler) const {
  return contains(this->packet_handlers, handler);
}

bool BackendCapabilities::supports_mtu(uint32_t mtu) const {
  if (!this->exact_mtu_values.empty()) {
    return contains(this->exact_mtu_values, mtu);
  }
  return mtu >= this->minimum_mtu && mtu <= this->maximum_mtu &&
         (!this->mtu_must_be_power_of_two || is_power_of_two(mtu));
}

bool BackendCapabilities::supports_rx_ring_entries(uint32_t entries) const {
  return is_power_of_two(entries) &&
         (this->maximum_rx_ring_entries == 0 ||
          entries <= this->maximum_rx_ring_entries);
}

bool BackendCapabilities::supports_mempool_cache(
    uint32_t configured_entries, uint32_t cache_entries) const {
  if (cache_entries > this->maximum_mempool_cache_size) return false;
  if (this->backend != Backend::kDpdk) return true;
  return static_cast<uint64_t>(cache_entries) * 3 / 2 <=
         this->usable_mempool_entries(configured_entries);
}

uint64_t BackendCapabilities::usable_mempool_entries(
    uint32_t configured_entries) const {
  if (configured_entries < this->unavailable_mempool_entries) return 0;
  return configured_entries - this->unavailable_mempool_entries;
}

const BackendCapabilities& capabilities_for(Backend backend) {
  switch (backend) {
    case Backend::kDpdk: return kDpdkCapabilities;
    case Backend::kRoce: return kRoceCapabilities;
  }
  throw std::invalid_argument("unknown Axio backend");
}

std::string_view dpdk_mempool_ops_name(MempoolHandler handler) {
  switch (handler) {
    case MempoolHandler::kRingMpMc: return "ring_mp_mc";
    case MempoolHandler::kRingSpSc: return "ring_sp_sc";
    case MempoolHandler::kRingMpSc: return "ring_mp_sc";
    case MempoolHandler::kRingSpMc: return "ring_sp_mc";
    case MempoolHandler::kRingMtRts: return "ring_mt_rts";
    case MempoolHandler::kRingMtHts: return "ring_mt_hts";
    case MempoolHandler::kStack: return "stack";
    case MempoolHandler::kLfStack: return "lf_stack";
    case MempoolHandler::kBucket: return "bucket";
    case MempoolHandler::kHugeAlloc: break;
  }
  throw std::invalid_argument("mempool handler is not implemented by DPDK");
}

}  // namespace axio::config
