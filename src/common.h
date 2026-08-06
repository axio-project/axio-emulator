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

#define AXIO_NODE_TYPE AXIO_SERVER
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
// #define AXIO_ROCE_MODE 0
#define AXIO_DPDK_MODE 1

#define AXIO_ROCE_UD 0
#define AXIO_ROCE_RC 1

#if defined(AXIO_ROCE_MODE) && AXIO_ROCE_MODE
  #define AXIO_ROCE_TRANSPORT_TYPE AXIO_ROCE_RC
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
 * ======================Quick test for the application======================
 */
/* -----Message-level specification----- */
#define AXIO_RX_MESSAGE_HANDLER kMessageHandlerThroughput
#define AXIO_APPLY_NEW_BUFFER 0
static constexpr size_t kAppTicksPerMsg = 0;    // extra execution ticks for each message, used for more accurate emulation
/// Payload size for AXIO_CLIENT behavior
// Corresponding MAC frame len: 22 -> 64; 86 -> 128; 214 -> 256; 470 -> 512; 982 -> 1024; 1458 -> 1500; 2002 -> 2048; 4054 -> 4096 (only for RC/DPDK)
constexpr size_t kAppReqPayloadSize = 
    (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerEmpty) ? 0 :
    (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerThroughput) ? 982 :
    (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerLatency) ? 86 :
    (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerMemory) ? 86 :
    (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerFileWrite) ? AXIO_KB(16) :
    (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerFileRead) ? 22 :
    (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerKeyValue) ?  81 : //type + key size + value size
    0;
static_assert(kAppReqPayloadSize > 0, "Invalid application payload size");
/// Payload size for AXIO_SERVER behavior
constexpr size_t kAppRespPayloadSize = 
    (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerEmpty) ? 0 :
    (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerThroughput) ? 22 :
    (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerLatency) ? 86 :
    (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerMemory) ? 86 :
    (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerFileWrite) ? 22 :
    (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerFileRead) ? AXIO_KB(100) :
    (AXIO_RX_MESSAGE_HANDLER == kMessageHandlerKeyValue) ? 81 : // type + key size + value size
    0;
static_assert(kAppRespPayloadSize > 0, "Invalid application response payload size");
// M_APP specific
static constexpr size_t kMemoryAccessRangePerPkt    = AXIO_KB(1);
static constexpr size_t kStatefulMemorySizePerCore  = AXIO_KB(256);

/* -----Packet-level specification----- */
#define AXIO_RX_PACKET_HANDLER kPacketHandlerEmpty

// Client-specific inflight-message budget. When disabled, the client sends as
// quickly as the datapath allows.
#define AXIO_ENABLE_INFLIGHT_LIMIT 1
static constexpr uint64_t kInflightMessageBudget = 1024;

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
