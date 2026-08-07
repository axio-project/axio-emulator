/**
 * @file backend_capabilities.h
 * @brief Backend-specific configuration capabilities.
 */
#pragma once

#include "axio/config/config_types.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace axio::config {

struct BackendCapabilities {
  Backend backend;
  std::vector<MempoolHandler> mempool_handlers;
  std::vector<PacketHandler> packet_handlers;
  std::vector<uint32_t> exact_mtu_values;
  uint32_t minimum_mtu;
  uint32_t maximum_mtu;
  bool mtu_must_be_power_of_two;
  // Zero means the limit depends on the selected device.
  uint32_t maximum_rx_ring_entries;
  uint32_t maximum_mempool_cache_size;
  uint32_t unavailable_mempool_entries;

  bool supports_mempool_handler(MempoolHandler handler) const;
  bool supports_packet_handler(PacketHandler handler) const;
  bool supports_mtu(uint32_t mtu) const;
  bool supports_rx_ring_entries(uint32_t entries) const;
  bool supports_mempool_cache(uint32_t configured_entries,
                              uint32_t cache_entries) const;
  uint64_t usable_mempool_entries(uint32_t configured_entries) const;
};

const BackendCapabilities& capabilities_for(Backend backend);
std::string_view dpdk_mempool_ops_name(MempoolHandler handler);

}  // namespace axio::config
