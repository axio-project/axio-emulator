/**
 * @file verbs_common.h
 * @brief Common definitions for ibverbs-based dispatchers
 */
#pragma once

#include <dirent.h>
#include <infiniband/verbs.h>
#include <string>
#include "dispatcher.h"
#include "util/logger.h"

namespace dperf {

// Constants for fast RECV driver mod
static constexpr uint64_t kMagicWrIDForFastRecv = 3185;
static constexpr uint64_t kModdedProbeWrID = 3186;
static constexpr int kModdedProbeRet = 3187;

/// Common information for verbs-based transports, resolved from a port ID
class VerbsResolve {
 public:
  int device_id = -1;  ///< Device index in list of verbs devices
  struct ibv_context *ib_ctx = nullptr;  ///< The verbs device context
  uint8_t dev_port_id = 0;  ///< 1-based port ID in device. 0 is invalid.
  size_t bandwidth = 0;     ///< Link bandwidth in bytes per second
};

static size_t enum_to_mtu(enum ibv_mtu mtu) {
  switch (mtu) {
    case IBV_MTU_256: return 256;
    case IBV_MTU_512: return 512;
    case IBV_MTU_1024: return 1024;
    case IBV_MTU_2048: return 2048;
    case IBV_MTU_4096: return 4096;
    default: return 0;
  }
}

static std::string link_layer_str(uint8_t link_layer) {
  switch (link_layer) {
    case IBV_LINK_LAYER_UNSPECIFIED: return "[Unspecified]";
    case IBV_LINK_LAYER_INFINIBAND: return "[InfiniBand]";
    case IBV_LINK_LAYER_ETHERNET: return "[Ethernet]";
    default: return "[Invalid]";
  }
}

/**
 * @brief A function wrapper whose \p pd argument is later bound to generate
 * this transport's \p reg_mr_func
 *
 * @throw runtime_error if memory registration fails
 */
// static Transport::mem_reg_info ibv_reg_mr_wrapper(struct ibv_pd *pd, void *buf,
//                                                   size_t size) {
//   struct ibv_mr *mr = ibv_reg_mr(pd, buf, size, IBV_ACCESS_LOCAL_WRITE);
//   rt_assert(mr != nullptr, "Failed to register mr.");

//   DPERF_INFO("Registered %zu MB (lkey = %u)\n", size / MB(1), mr->lkey);
//   return Transport::mem_reg_info(mr, mr->lkey);
// }

/// A function wrapper used to generate a verbs transport's memory
/// deregistration function
// static void ibv_dereg_mr_wrapper(Transport::mem_reg_info mr) {
//   auto *ib_mr = reinterpret_cast<struct ibv_mr *>(mr.transport_mr_);
//   size_t size = ib_mr->length;
//   uint32_t lkey = ib_mr->lkey;

//   int ret = ibv_dereg_mr(ib_mr);
//   if (ret != 0) {
//     DPERF_ERROR("Memory degistration failed. size %zu B, lkey %u\n",
//                size / MB(1), lkey);
//   }

//   DPERF_INFO("Deregistered %zu MB (lkey = %u)\n", size / MB(1), lkey);
// }

/// Polls a CQ for one completion. In verbose mode only, prints a warning
/// message if polling gets stuck.
static inline void poll_cq_one_helper(struct ibv_cq *cq) {
  struct ibv_wc wc;
  size_t num_tries = 0;
  while (ibv_poll_cq(cq, 1, &wc) == 0) {
    // Do nothing while we have no CQE or poll_cq error
    if (DPERF_LOG_LEVEL == DPERF_LOG_LEVEL_INFO) {
      num_tries++;
      if (unlikely(num_tries == GB(1))) {
        fprintf(stderr, "DPerf: Warning. Stuck in poll_cq().");
        num_tries = 0;
      }
    }
  }

  if (unlikely(wc.status != 0)) {
    fprintf(stderr, "DPerf: Fatal error. Bad wc status %d.\n", wc.status);
    assert(false);
    exit(-1);
  }
}

/// Return the net interface for a verbs device (e.g., mlx5_0 -> enp4s0f0)
static std::string ibdev2netdev(std::string ibdev_name) {
  std::string dev_dir = "/sys/class/infiniband/" + ibdev_name + "/device/net";

  std::vector<std::string> net_ifaces;
  DIR *dp;
  struct dirent *dirp;
  dp = opendir(dev_dir.c_str());
  rt_assert(dp != nullptr, "Failed to open directory " + dev_dir);

  while (true) {
    dirp = readdir(dp);
    if (dirp == nullptr) break;

    if (strcmp(dirp->d_name, ".") == 0) continue;
    if (strcmp(dirp->d_name, "..") == 0) continue;
    net_ifaces.push_back(std::string(dirp->d_name));
  }
  closedir(dp);

  rt_assert(net_ifaces.size() > 0, "Directory " + dev_dir + " is empty");
  return net_ifaces[0];
}

static void common_resolve_phy_port(char *dev_name, uint8_t phy_port,
                                    size_t mtu, VerbsResolve &resolve) {
    std::ostringstream xmsg;  // The exception message
    int dev_idx;
    int num_devices = 0;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    rt_assert(dev_list != nullptr, "Failed to get device list");

    // Traverse the device list
    for (dev_idx = 0; dev_list[dev_idx] != nullptr; num_devices++) {
        if (!strcmp(ibv_get_device_name(dev_list[dev_idx]), dev_name))
            break;
        else
            dev_idx++;
    }
    if (dev_list[dev_idx] == nullptr) {
        ibv_free_device_list(dev_list);
        xmsg << "Device " << dev_name << " not found";
        throw std::runtime_error(xmsg.str());
    }
    // Open the device and query its ports
    struct ibv_context *ib_ctx = ibv_open_device(dev_list[dev_idx]);
    rt_assert(ib_ctx != nullptr,
              "Failed to open dev " + std::to_string(dev_idx));
    struct ibv_device_attr device_attr;
    memset(&device_attr, 0, sizeof(device_attr));
    if (ibv_query_device(ib_ctx, &device_attr) != 0) {
        xmsg << "Failed to query device " << std::to_string(dev_idx);
        throw std::runtime_error(xmsg.str());
    }
    struct ibv_port_attr port_attr;
    if (ibv_query_port(ib_ctx, phy_port + 1, &port_attr) != 0) {
        xmsg << "Failed to query port " << std::to_string(phy_port)
             << " on device " << ib_ctx->device->name;
        throw std::runtime_error(xmsg.str());
    }
    if (port_attr.phys_state != IBV_PORT_ACTIVE &&
        port_attr.phys_state != IBV_PORT_ACTIVE_DEFER) {
      xmsg << "Port " << std::to_string(phy_port) << " is not active";
      throw std::runtime_error(xmsg.str());
    }
    const auto expected_link_layer = IBV_LINK_LAYER_ETHERNET; // Only support RoCEv2 
    if (port_attr.link_layer != expected_link_layer) {
      throw std::runtime_error("Invalid link layer. Port link layer is " +
                                link_layer_str(port_attr.link_layer));
    }

    // Check the MTU
    size_t active_mtu = enum_to_mtu(port_attr.active_mtu);
    if (mtu > active_mtu) {
      throw std::runtime_error("Transport's required MTU is " +
                                std::to_string(mtu) + ", active_mtu is " +
                                std::to_string(active_mtu));
    }

    resolve.device_id = dev_idx;
    resolve.ib_ctx = ib_ctx;
    resolve.dev_port_id = phy_port + 1;

    // Compute the bandwidth
    double gbps_per_lane = -1;
    switch (port_attr.active_speed) {
      case 1: gbps_per_lane = 2.5; break;
      case 2: gbps_per_lane = 5.0; break;
      case 4: gbps_per_lane = 10.0; break;
      case 8: gbps_per_lane = 10.0; break;
      case 16: gbps_per_lane = 14.0; break;
      case 32: gbps_per_lane = 25.0; break;
      case 64: gbps_per_lane = 50.0; break;
      case 128: gbps_per_lane = 100.0; break;
      default: rt_assert(false, "Invalid active speed, active speed: " + std::to_string(port_attr.active_speed));
    };

    size_t num_lanes = SIZE_MAX;
    switch (port_attr.active_width) {
      case 1: num_lanes = 1; break;
      case 2: num_lanes = 4; break;
      case 4: num_lanes = 8; break;
      case 8: num_lanes = 12; break;
      default: rt_assert(false, "Invalid active width");
    };

    double total_gbps = num_lanes * gbps_per_lane;
    resolve.bandwidth = total_gbps * (1000 * 1000 * 1000) / 8.0;

    DPERF_INFO(
        "Port %u resolved to device %s. Speed = %.2f Gbps.\n",
        phy_port, ib_ctx->device->name, total_gbps);

    // If we are here, port resolution has failed
    rt_assert(resolve.ib_ctx != nullptr,
              "Failed to resolve port " + std::to_string(phy_port));
}

}