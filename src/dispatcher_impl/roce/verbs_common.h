/**
 * @file verbs_common.h
 * @brief Common definitions for ibverbs-based dispatchers.
 */
#pragma once

#include "dispatcher.h"
#include "util/logger.h"

#include <infiniband/verbs.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

namespace axio {

/** Passive result of resolving a configured verbs device and port. */
struct VerbsResolve {
  int device_id_ = -1;
  ibv_context* context_ = nullptr;
  uint8_t port_id_ = 0;
  size_t bandwidth_bytes_per_second_ = 0;
};

static inline size_t verbs_mtu_bytes(enum ibv_mtu mtu) {
  switch (mtu) {
    case IBV_MTU_256:
      return 256;
    case IBV_MTU_512:
      return 512;
    case IBV_MTU_1024:
      return 1024;
    case IBV_MTU_2048:
      return 2048;
    case IBV_MTU_4096:
      return 4096;
    default:
      return 0;
  }
}

static inline std::string verbs_link_layer_name(uint8_t link_layer) {
  switch (link_layer) {
    case IBV_LINK_LAYER_UNSPECIFIED:
      return "[Unspecified]";
    case IBV_LINK_LAYER_INFINIBAND:
      return "[InfiniBand]";
    case IBV_LINK_LAYER_ETHERNET:
      return "[Ethernet]";
    default:
      return "[Invalid]";
  }
}

static inline void resolve_verbs_port(const char* device_name,
                                      uint8_t physical_port, size_t mtu,
                                      VerbsResolve& resolved_port) {
  std::ostringstream error_message;
  int device_count = 0;
  ibv_device** device_list = ibv_get_device_list(&device_count);
  rt_assert(device_list != nullptr, "Failed to get device list");

  int device_index = 0;
  while (device_list[device_index] != nullptr &&
         strcmp(ibv_get_device_name(device_list[device_index]), device_name) !=
             0) {
    device_index++;
  }
  if (device_list[device_index] == nullptr) {
    ibv_free_device_list(device_list);
    error_message << "Device " << device_name << " not found";
    throw std::runtime_error(error_message.str());
  }

  ibv_context* context = ibv_open_device(device_list[device_index]);
  rt_assert(context != nullptr,
            "Failed to open device " + std::to_string(device_index));

  struct ibv_device_attr device_attributes;
  memset(&device_attributes, 0, sizeof(device_attributes));
  if (ibv_query_device(context, &device_attributes) != 0) {
    error_message << "Failed to query device " << device_index;
    throw std::runtime_error(error_message.str());
  }

  struct ibv_port_attr port_attributes;
  if (ibv_query_port(context, physical_port + 1, &port_attributes) != 0) {
    error_message << "Failed to query port "
                  << std::to_string(physical_port) << " on device "
                  << context->device->name;
    throw std::runtime_error(error_message.str());
  }
  if (port_attributes.phys_state != IBV_PORT_ACTIVE &&
      port_attributes.phys_state != IBV_PORT_ACTIVE_DEFER) {
    error_message << "Port " << std::to_string(physical_port)
                  << " is not active";
    throw std::runtime_error(error_message.str());
  }

  const auto expected_link_layer = IBV_LINK_LAYER_ETHERNET;
  if (port_attributes.link_layer != expected_link_layer) {
    throw std::runtime_error(
        "Invalid link layer. Port link layer is " +
        verbs_link_layer_name(port_attributes.link_layer));
  }

  size_t active_mtu = verbs_mtu_bytes(port_attributes.active_mtu);
  if (mtu > active_mtu) {
    throw std::runtime_error("Transport's required MTU is " +
                             std::to_string(mtu) + ", active_mtu is " +
                             std::to_string(active_mtu));
  }

  resolved_port.device_id_ = device_index;
  resolved_port.context_ = context;
  resolved_port.port_id_ = physical_port + 1;

  double gigabits_per_second_per_lane = -1;
  switch (port_attributes.active_speed) {
    case 1:
      gigabits_per_second_per_lane = 2.5;
      break;
    case 2:
      gigabits_per_second_per_lane = 5.0;
      break;
    case 4:
    case 8:
      gigabits_per_second_per_lane = 10.0;
      break;
    case 16:
      gigabits_per_second_per_lane = 14.0;
      break;
    case 32:
      gigabits_per_second_per_lane = 25.0;
      break;
    case 64:
      gigabits_per_second_per_lane = 50.0;
      break;
    case 128:
      gigabits_per_second_per_lane = 100.0;
      break;
    default:
      rt_assert(false,
                "Invalid active speed: " +
                    std::to_string(port_attributes.active_speed));
  }

  size_t lane_count = SIZE_MAX;
  switch (port_attributes.active_width) {
    case 1:
      lane_count = 1;
      break;
    case 2:
      lane_count = 4;
      break;
    case 4:
      lane_count = 8;
      break;
    case 8:
      lane_count = 12;
      break;
    default:
      rt_assert(false, "Invalid active width");
  }

  double total_gigabits_per_second =
      lane_count * gigabits_per_second_per_lane;
  resolved_port.bandwidth_bytes_per_second_ =
      total_gigabits_per_second * (1000 * 1000 * 1000) / 8.0;

  AXIO_INFO("Port %u resolved to device %s. Speed = %.2f Gbps.\n",
            physical_port, context->device->name,
            total_gigabits_per_second);
  rt_assert(resolved_port.context_ != nullptr,
            "Failed to resolve port " + std::to_string(physical_port));
}

}  // namespace axio
