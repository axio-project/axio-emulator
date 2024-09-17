/**
 * @file dispatcher.h
 * @brief General definitions for all dispatcher types. A Dispather instance encapsulate driver codes.
 */
#pragma once
#include "common.h"
#include "util/math_utils.h"
#include "dispatcher_impl/ethhdr.h"
#include "dispatcher_impl/iphdr.h"
#include "ws_impl/ws_hdr.h"
#include "config.h"

#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include <functional>
#include <unordered_map>

namespace dperf {
/// The avialable transport backend implementations.
enum class DispatcherType { kDPDK,kRoCE };

/// Generic dispatcher class defination
class Dispatcher {
  /**
   * ----------------------Parameters tuned by PipeTune----------------------
   */ 
  public:
    /// Minimal number of buffered packets for collect_tx_pkts
    uint16_t kDispTxBatchSize = 0;
    /// Maximum number of transmitted packets for tx_burst
    uint16_t kDispRxBatchSize = 0;
    /// Minimal number of buffered packets before dispatching
    uint16_t kNICTxPostSize = 0;
    /// Maximum number of packets received in rx_burst
    uint16_t kNICRxPostSize = 0;
  /**
   * ----------------------Parameters in dispatcher level----------------------
   */ 
    static constexpr size_t kNumRxRingEntries = 2048;
    static_assert(is_power_of_two<size_t>(kNumRxRingEntries), "The num of RX ring entries is not power of two.");
    static constexpr size_t kNumTxRingEntries = 2048;
    static_assert(is_power_of_two<size_t>(kNumTxRingEntries), "The num of TX ring entries is not power of two.");
    static constexpr size_t kSizeMemPool = 8192;  // mbuf pool size
    static constexpr size_t kMTU = 1024;
    static_assert(is_power_of_two<size_t>(kMTU), "The size of MTU is not power of two.");
    static constexpr size_t kMaxPayloadSize = kMTU - sizeof(iphdr) - sizeof(udphdr);
    static constexpr uint16_t kDefaultUdpPort = 10010;
    const char* kLocalIpStr;
    const char* kRemoteIpStr;
    eth_addr kLocalMac;
    eth_addr kRemoteMac;

  /**
   * ----------------------Dispatcher internal structures----------------------
   */ 
  public:
    /// Generic struct to store memory registration info for any dispatcher.
    template <typename T>
    struct mem_reg_info {
      void* dispatcher_mr_;     ///< The dispatcher-specific memory region (eg, ibv_mr, dpdk mempool)
      using alloc_t = T*(*)(void*);
      using alloc_bulk_t = uint8_t(*)(void*, T**, size_t);
      using de_alloc_t = void(*)(T*, void*);
      using de_alloc_bulk_t = void(*)(T**, size_t, void*);
      using set_payload_t = void(*)(T*, char*, char*, size_t);
      using extract_ws_hdr_t = ws_hdr*(*)(T*);
      using cp_payload_t = void(*)(T*, T*, char*, char*, size_t);

      alloc_t alloc_;
      de_alloc_t de_alloc_;
      alloc_bulk_t alloc_bulk_;
      de_alloc_bulk_t de_alloc_bulk_;
      set_payload_t set_payload_;
      extract_ws_hdr_t extract_ws_hdr_;
      cp_payload_t cp_payload_;
      

      // Constructor to initialize the function pointer
      mem_reg_info(
        void* mr, alloc_t alloc,
        de_alloc_t de_alloc,
        alloc_bulk_t alloc_bulk,
        de_alloc_bulk_t de_alloc_bulk,
        set_payload_t set_payload,
        extract_ws_hdr_t extract_ws_hdr,
        cp_payload_t cp_payload
      ) 
        : dispatcher_mr_(mr), alloc_(alloc), de_alloc_(de_alloc), 
        alloc_bulk_(alloc_bulk), de_alloc_bulk_(de_alloc_bulk), set_payload_(set_payload),
        extract_ws_hdr_(extract_ws_hdr), cp_payload_(cp_payload) {}
    };


  /**
   * ----------------------Dispatcher methods----------------------
   */ 
  public:
    /**
     * @brief Base class definiation of Dispatcher.
     *
     * @param phy_port An Workspace object uses one port on a "datapath" NIC, which
     * refers to a NIC that supports DPDK. phy_port is the zero-based index of 
     * that port among active ports, same as the one passed to
     * `rte_eth_dev_info_get` for the DPDK transpor. Multiple Workspace objects may 
     * use the same phy_port.
     *
     * @throw runtime_error if construction fails
     */
    Dispatcher(DispatcherType, uint8_t ws_id, uint8_t phy_port, size_t numa_node, UserConfig *user_config); // Fake dispatcher init
    ~Dispatcher();

  /**
   * ----------------------Util methods----------------------
   */ 
  public:
    static std::string get_name(DispatcherType transport_type) {
      switch (transport_type) {
        case DispatcherType::kDPDK: return "[DPDK]";
        case DispatcherType::kRoCE: return "[RoCE]";
      }
      throw std::runtime_error("eRPC: Invalid transport");
    }

  /**
   * ----------------------Internal Parameters----------------------
   */   
  public:
    const DispatcherType dispatcher_type_;
    const uint8_t phy_port_;  ///< 0-based index among active fabric ports  
    const size_t numa_node_;

};
}

/**
 * ----------------------Include sub-class of Dispatcher----------------------
 */ 
#ifdef RoceMode
  #include "dispatcher_impl/roce/roce_dispatcher.h"
#elif DpdkMode
  #include "dispatcher_impl/dpdk/dpdk_dispatcher.h"
#endif

