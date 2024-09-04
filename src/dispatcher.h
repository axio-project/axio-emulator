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
   * ----------------------Parameters in dispatcher level----------------------
   */ 
  public:
    static constexpr size_t kNumRxRingEntries = 2048;
    static_assert(is_power_of_two<size_t>(kNumRxRingEntries), "The num of RX ring entries is not power of two.");
    static constexpr size_t kNumTxRingEntries = 2048;
    static_assert(is_power_of_two<size_t>(kNumTxRingEntries), "The num of TX ring entries is not power of two.");
    static constexpr size_t kSizeMemPool = 8192;  // mbuf pool size
    static constexpr size_t kMTU = 1024;
    static_assert(is_power_of_two<size_t>(kMTU), "The size of MTU is not power of two.");
    static constexpr size_t kMaxPayloadSize = kMTU - sizeof(iphdr) - sizeof(udphdr);
    /// Minimal number of buffered packets for collect_tx_pkts
    static constexpr size_t kTxBatchSize = 32;
    /// Maximum number of transmitted packets for tx_burst
    static constexpr size_t kTxPostSize = 32;
    /// Minimal number of buffered packets before dispatching
    static constexpr size_t kRxBatchSize = 32;
    /// Maximum number of packets received in rx_burst
    static constexpr size_t kRxPostSize = 128;
    static constexpr uint16_t kDefaultUdpPort = 10010;
    const char* kLocalIpStr = "10.0.4.102";
    const char* kRemoteIpStr = "10.0.4.101";
    const char* kTaccIP_0 = "10.2.15.1";
    const char* kTaccIP_1 = "10.2.13.1";
    const eth_addr kSwitchMac = {0x1c, 0x34, 0xda, 0xf3, 0x99, 0xc8};
    const eth_addr kLocalMac = {0xa0, 0x88, 0xc2, 0xbf, 0x9b, 0x10};
    const eth_addr kRemoteMac = {0xa0, 0x88, 0xc2, 0xbf, 0x46, 0x4e};

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
    Dispatcher(DispatcherType, uint8_t ws_id, uint8_t phy_port, size_t numa_node); // Fake dispatcher
    ~Dispatcher();

  /**
   * ----------------------Util methods----------------------
   */ 
  public:
    static std::string get_name(DispatcherType transport_type) {
      switch (transport_type) {
        case DispatcherType::kDPDK: return "[DPDK]";
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

