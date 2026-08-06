/**
 * @file common.h
 * @brief Common header file with convenience definitions
 */
#pragma once

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cerrno>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cmath>

namespace axio {

#define AXIO_UNUSED(x) ((void)(x))  // Make production build happy
#define AXIO_LIKELY(x) __builtin_expect(!!(x), 1)
#define AXIO_UNLIKELY(x) __builtin_expect(!!(x), 0)

#define AXIO_KB(x) (static_cast<size_t>(x) << 10)
#define AXIO_MB(x) (static_cast<size_t>(x) << 20)
#define AXIO_GB(x) (static_cast<size_t>(x) << 30)

#define AXIO_CEIL_POWER_OF_TWO(x)    std::pow(2, std::ceil(std::log(x)/std::log(2)))

/**
 * ----------------------Server constants----------------------
 */ 
static constexpr size_t kMaxPhyPorts = 2;
static constexpr size_t kMaxNumaNodes = 2;
static constexpr size_t kMaxQueuesPerPort = 16;
static constexpr size_t kHugepageSize = (2 * 1024 * 1024);  ///< Hugepage size

/**
 * ----------------------Perf Test constants----------------------
 */ 
#define AXIO_PERF_TEST 1       // 1: enable perf test, 0: disable perf test
#define AXIO_PERF_TEST_LATENCY 1
#define AXIO_PERF_TEST_THROUGHPUT 1
#define AXIO_PERF_TEST_LATENCY_MIN_MAX 1
#define AXIO_PERF_TEST_MBUF_RANGE 1
#define AXIO_LATENCY_SAMPLE_STRIDE 4096        // Sample once every 4096 packets
#define AXIO_LATENCY_SAMPLE_COUNT 1024    // Sample 1024 packets

// optimized latency measurement
#define AXIO_LATENCY_USE_RDTSCP 1           // use RDTSCP to improve precision

/**
 * ----------------------Node Type----------------------
 */
#define AXIO_CLIENT 0
#define AXIO_SERVER 1

#ifndef AXIO_CONFIG_SCHEMA_VERSION
#define AXIO_CONFIG_SCHEMA_VERSION 1
#endif
#ifndef AXIO_CONFIG_NODE_TYPE
#define AXIO_CONFIG_NODE_TYPE AXIO_SERVER
#endif
#define AXIO_NODE_TYPE AXIO_CONFIG_NODE_TYPE
#define AXIO_ENABLE_TUNING 0
#define AXIO_ENABLE_TESTS 0

/**
 * ----------------------App behaviour control----------------------
 */
enum MessageHandlerType : uint8_t {
  kMessageHandlerEmpty = 0,
  kMessageHandlerThroughput,
  kMessageHandlerLatency,
  kMessageHandlerMemory,
  kMessageHandlerFileWrite,
  kMessageHandlerFileRead,
  kMessageHandlerKeyValue
};

/**
 * ----------------------Dispatcher modes----------------------
 */ 
#define AXIO_ROCE_UD 0
#define AXIO_ROCE_RC 1

#ifndef AXIO_CONFIG_DPDK_MODE
#define AXIO_CONFIG_DPDK_MODE 1
#endif
#ifndef AXIO_CONFIG_ROCE_MODE
#define AXIO_CONFIG_ROCE_MODE 0
#endif
#ifndef AXIO_CONFIG_ROCE_TRANSPORT_TYPE
#define AXIO_CONFIG_ROCE_TRANSPORT_TYPE AXIO_ROCE_RC
#endif

#define AXIO_DPDK_MODE AXIO_CONFIG_DPDK_MODE
#define AXIO_ROCE_MODE AXIO_CONFIG_ROCE_MODE
#define AXIO_ROCE_TRANSPORT_TYPE AXIO_CONFIG_ROCE_TRANSPORT_TYPE

#if (AXIO_DPDK_MODE + AXIO_ROCE_MODE) != 1
#error "Select exactly one Axio dispatcher backend"
#endif

#if defined(AXIO_ROCE_MODE) && AXIO_ROCE_MODE
  #define AXIO_DISPATCHER_TYPE RoceDispatcher
  #define AXIO_MEMORY_BUFFER_TYPE Buffer
#elif defined(AXIO_DPDK_MODE) && AXIO_DPDK_MODE
  #define AXIO_DISPATCHER_TYPE DpdkDispatcher
  #define AXIO_MEMORY_BUFFER_TYPE rte_mbuf
#else
  #error "Select exactly one Axio dispatcher backend"
#endif

enum PacketHandlerType : uint8_t {
  kPacketHandlerEmpty = 0,
  kPacketHandlerEcho
};

/**
 * Build-time values default to the current 1.1.3 behavior. A generated header
 * may define any AXIO_CONFIG_* macro before this file is included.
 */
#ifndef AXIO_CONFIG_MTU
#define AXIO_CONFIG_MTU 2048
#endif
#ifndef AXIO_CONFIG_RX_RING_ENTRIES
#define AXIO_CONFIG_RX_RING_ENTRIES 2048
#endif
#ifndef AXIO_CONFIG_TX_RING_ENTRIES
#define AXIO_CONFIG_TX_RING_ENTRIES 2048
#endif
#ifndef AXIO_CONFIG_MEMPOOL_SIZE
#define AXIO_CONFIG_MEMPOOL_SIZE 8192
#endif
#ifndef AXIO_CONFIG_MEMPOOL_HANDLER
#define AXIO_CONFIG_MEMPOOL_HANDLER 0
#endif
#ifndef AXIO_CONFIG_MEMPOOL_HANDLER_NAME
#define AXIO_CONFIG_MEMPOOL_HANDLER_NAME "ring_mp_mc"
#endif
#ifndef AXIO_CONFIG_MEMPOOL_CACHE_SIZE
  #if AXIO_NODE_TYPE == AXIO_CLIENT
    #define AXIO_CONFIG_MEMPOOL_CACHE_SIZE 512
  #else
    #define AXIO_CONFIG_MEMPOOL_CACHE_SIZE 0
  #endif
#endif

/**
 * ======================Quick test for the application======================
 */
/* -----Message-level specification----- */
#ifndef AXIO_CONFIG_MESSAGE_HANDLER
#define AXIO_CONFIG_MESSAGE_HANDLER 1  // kMessageHandlerThroughput
#endif
#ifndef AXIO_CONFIG_APPLY_NEW_MBUF
#define AXIO_CONFIG_APPLY_NEW_MBUF 0
#endif
#ifndef AXIO_CONFIG_APP_TICKS_PER_MESSAGE
#define AXIO_CONFIG_APP_TICKS_PER_MESSAGE 0
#endif

#define AXIO_RX_MESSAGE_HANDLER \
  static_cast<::axio::MessageHandlerType>(AXIO_CONFIG_MESSAGE_HANDLER)
#define AXIO_APPLY_NEW_BUFFER AXIO_CONFIG_APPLY_NEW_MBUF
static constexpr size_t kAppTicksPerMsg = AXIO_CONFIG_APP_TICKS_PER_MESSAGE;
/// Payload size for AXIO_CLIENT behavior
// Corresponding MAC frame len: 22 -> 64; 86 -> 128; 214 -> 256; 470 -> 512; 982 -> 1024; 1458 -> 1500; 2002 -> 2048; 4054 -> 4096 (only for RC/DPDK)
#ifndef AXIO_CONFIG_REQUEST_PAYLOAD_BYTES
#define AXIO_CONFIG_REQUEST_PAYLOAD_BYTES \
    ((AXIO_CONFIG_MESSAGE_HANDLER == 0) ? 0 : \
     (AXIO_CONFIG_MESSAGE_HANDLER == 1) ? 982 : \
     (AXIO_CONFIG_MESSAGE_HANDLER == 2) ? 86 : \
     (AXIO_CONFIG_MESSAGE_HANDLER == 3) ? 86 : \
     (AXIO_CONFIG_MESSAGE_HANDLER == 4) ? AXIO_KB(16) : \
     (AXIO_CONFIG_MESSAGE_HANDLER == 5) ? 22 : \
     (AXIO_CONFIG_MESSAGE_HANDLER == 6) ? 81 : 0)
#endif
constexpr size_t kAppReqPayloadSize = AXIO_CONFIG_REQUEST_PAYLOAD_BYTES;
static_assert(kAppReqPayloadSize > 0, "Invalid application payload size");
/// Payload size for AXIO_SERVER behavior
#ifndef AXIO_CONFIG_RESPONSE_PAYLOAD_BYTES
#define AXIO_CONFIG_RESPONSE_PAYLOAD_BYTES \
    ((AXIO_CONFIG_MESSAGE_HANDLER == 0) ? 0 : \
     (AXIO_CONFIG_MESSAGE_HANDLER == 1) ? 22 : \
     (AXIO_CONFIG_MESSAGE_HANDLER == 2) ? 86 : \
     (AXIO_CONFIG_MESSAGE_HANDLER == 3) ? 86 : \
     (AXIO_CONFIG_MESSAGE_HANDLER == 4) ? 22 : \
     (AXIO_CONFIG_MESSAGE_HANDLER == 5) ? AXIO_KB(100) : \
     (AXIO_CONFIG_MESSAGE_HANDLER == 6) ? 81 : 0)
#endif
constexpr size_t kAppRespPayloadSize = AXIO_CONFIG_RESPONSE_PAYLOAD_BYTES;
static_assert(kAppRespPayloadSize > 0, "Invalid application response payload size");
// M_APP specific
static constexpr size_t kMemoryAccessRangePerPkt    = AXIO_KB(1);
static constexpr size_t kStatefulMemorySizePerCore  = AXIO_KB(256);

/* -----Packet-level specification----- */
#ifndef AXIO_CONFIG_PACKET_HANDLER
#define AXIO_CONFIG_PACKET_HANDLER 0  // kPacketHandlerEmpty
#endif
#define AXIO_RX_PACKET_HANDLER \
  static_cast<::axio::PacketHandlerType>(AXIO_CONFIG_PACKET_HANDLER)

// Client-specific inflight-message budget. When disabled, the client sends as
// quickly as the datapath allows.
#ifndef AXIO_CONFIG_INFLIGHT_LIMIT_ENABLED
#define AXIO_CONFIG_INFLIGHT_LIMIT_ENABLED 1
#endif
#ifndef AXIO_CONFIG_INFLIGHT_MESSAGES
#define AXIO_CONFIG_INFLIGHT_MESSAGES 1024
#endif
#define AXIO_ENABLE_INFLIGHT_LIMIT AXIO_CONFIG_INFLIGHT_LIMIT_ENABLED
static constexpr uint64_t kInflightMessageBudget =
    AXIO_CONFIG_INFLIGHT_MESSAGES;

/**
 * ----------------------AXIO_ONE_STAGE modes----------------------
 */
/// Available types: kTxNicPhase, kTxDispatcherPhase, kTxApplicationPhase, kRxNicPhase, kRxDispatcherPhase, kRxApplicationPhase
// #define AXIO_ONE_STAGE kRxApplicationPhase
/*!
 *  \note [xinyang] for app and dispatcher stage, max value is kWsQueueSize - 1, for nic stage, max 
                    value is kNumTxRingEntries
 *  \note [zhuobin] if this number exceed the size of mempool cache size, then the apply_bulk will
 *                  leak to the normal mempool mbufs, which might cause high cache miss rate
 */
#define AXIO_FLOW_SIZE 256

/**
 * ----------------------General constants----------------------
 */ 

#define AXIO_LOG_LEVEL 3

static constexpr uint8_t kWorkspaceTypeNum = 3;
static constexpr uint8_t kInvalidWorkspaceType = uint8_t{1} << kWorkspaceTypeNum;
static constexpr uint8_t kWorkspaceMaxNum = 16;
static constexpr uint16_t kMaxBatchSize = 512;
static constexpr uint8_t kInvalidWsId = kWorkspaceMaxNum + 1;
static constexpr size_t  kWsQueueSize = 4096;    // Queue size must be power of two

/// Parameters for datapath pipeline
static constexpr uint8_t kMaxWorkloadNum = kWorkspaceMaxNum;
static constexpr uint8_t kInvalidWorkloadType = kMaxWorkloadNum + 1;

static constexpr uint8_t kTxNicPhase = 0;
static constexpr uint8_t kTxDispatcherPhase = 1;
static constexpr uint8_t kTxApplicationPhase = 2;
static constexpr uint8_t kRxNicPhase = 3;
static constexpr uint8_t kRxDispatcherPhase = 4;
static constexpr uint8_t kRxApplicationPhase = 5;

/**
 * ----------------------Simple methods----------------------
 */ 
static inline void rt_assert(bool condition, std::string throw_str, char *s) {
  if (AXIO_UNLIKELY(!condition)) {
    fprintf(stderr, "%s %s\n", throw_str.c_str(), s);
    exit(-1);
  }
}

static inline void rt_assert(bool condition, const char *throw_str) {
  if (AXIO_UNLIKELY(!condition)) {
    fprintf(stderr, "%s\n", throw_str);
    exit(-1);
  }
}

static inline void rt_assert(bool condition, std::string throw_str) {
  if (AXIO_UNLIKELY(!condition)) {
    fprintf(stderr, "%s\n", throw_str.c_str());
    exit(-1);
  }
}

static inline void rt_assert(bool condition) {
  if (AXIO_UNLIKELY(!condition)) {
    fprintf(stderr, "Error\n");
    assert(false);
    exit(-1);
  }
}

/// Check a condition at runtime. If the condition is false, print error message
/// and exit.
static inline void exit_assert(bool condition, std::string error_msg) {
  if (AXIO_UNLIKELY(!condition)) {
    fprintf(stderr, "%s. Exiting.\n", error_msg.c_str());
    fflush(stderr);
    exit(-1);
  }
}

/**
 * ----------------------Print related----------------------
 */ 
const std::string kAnsiReset = "\033[0m";
const std::string kAnsiRed = "\033[31m";
const std::string kAnsiGreen = "\033[32m";
const std::string kAnsiYellow = "\033[33m";
const std::string kAnsiBlue = "\033[34m";

}  // namespace axio
