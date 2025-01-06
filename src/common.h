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

namespace dperf {

#define _unused(x) ((void)(x))  // Make production build happy
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define KB(x) (static_cast<size_t>(x) << 10)
#define MB(x) (static_cast<size_t>(x) << 20)
#define GB(x) (static_cast<size_t>(x) << 30)

#define CEIL_2(x)    std::pow(2, std::ceil(std::log(x)/std::log(2)))

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
#define PERF_TEST 1       // 1: enable perf test, 0: disable perf test
#define PERF_TEST_LAT 1
#define PERF_TEST_THR 1
#define PERF_TEST_LAT_MIN_MAX 1
#define PERT_TEST_MBUF_RANGE 1
#define PERF_LAT_SAMPLE_STRIP 4096        // Sample once every 4096 packets
#define PERF_LAT_SAMPLE_NUM 1024    // Sample 1024 packets

/**
 * ----------------------Node Type----------------------
 */
#define CLIENT 0
#define SERVER 1

#define NODE_TYPE SERVER
#define ENABLE_TUNE false

/**
 * ----------------------App behaviour control----------------------
 */
enum msg_handler_type_t : uint8_t {
  kRxMsgHandler_Empty = 0,
  kRxMsgHandler_T_APP,
  kRxMsgHandler_L_APP,
  kRxMsgHandler_M_APP,
  kRxMsgHandler_FS_WRITE,
  kRxMsgHandler_FS_READ,
  kRxMsgHandler_KV
};

/**
 * ----------------------Dispatcher modes----------------------
 */ 
#define RoceMode 0
// #define DpdkMode 1

#define UD 0
#define RC 1

#ifdef RoceMode
  #define RoCE_TYPE UD
  #define DISPATCHER_TYPE RoceDispatcher
  #define MEM_REG_TYPE Buffer
#elif DpdkMode
  #define DISPATCHER_TYPE DpdkDispatcher
  #define MEM_REG_TYPE rte_mbuf
#endif

enum pkt_handler_type_t : uint8_t {
  kRxPktHandler_Empty = 0,
  kRxPktHandler_Echo
};

/**
 * ======================Quick test for the application======================
 */
/* -----Message-level specification----- */
#define kRxMsgHandler kRxMsgHandler_T_APP
#define ApplyNewMbuf false
static constexpr size_t kAppTicksPerMsg = 0;    // extra execution ticks for each message, used for more accurate emulation
/// Payload size for CLIENT behavior
// Corresponding MAC frame len: 22 -> 64; 86 -> 128; 214 -> 256; 470 -> 512; 982 -> 1024; 1458 -> 1500
constexpr size_t kAppReqPayloadSize = 
    (kRxMsgHandler == kRxMsgHandler_Empty) ? 0 :
    (kRxMsgHandler == kRxMsgHandler_T_APP) ? 982 :
    (kRxMsgHandler == kRxMsgHandler_L_APP) ? 86 :
    (kRxMsgHandler == kRxMsgHandler_M_APP) ? 86 : 
    (kRxMsgHandler == kRxMsgHandler_FS_WRITE) ? KB(16) : 
    (kRxMsgHandler == kRxMsgHandler_FS_READ) ? 22 : 
    (kRxMsgHandler == kRxMsgHandler_KV) ?  81 : //type + key size + value size
    0;
static_assert(kAppReqPayloadSize > 0, "Invalid application payload size");
/// Payload size for SERVER behavior
constexpr size_t kAppRespPayloadSize = 
    (kRxMsgHandler == kRxMsgHandler_Empty) ? 0 :
    (kRxMsgHandler == kRxMsgHandler_T_APP) ? 22 :
    (kRxMsgHandler == kRxMsgHandler_L_APP) ? 86 :
    (kRxMsgHandler == kRxMsgHandler_M_APP) ? 86 : 
    (kRxMsgHandler == kRxMsgHandler_FS_WRITE) ? 22 : 
    (kRxMsgHandler == kRxMsgHandler_FS_READ) ? KB(100) : 
    (kRxMsgHandler == kRxMsgHandler_KV) ? 81 : // type + key size + value size
    0;
static_assert(kAppRespPayloadSize > 0, "Invalid application response payload size");
// M_APP specific
static constexpr size_t kMemoryAccessRangePerPkt    = KB(1);
static constexpr size_t kStatefulMemorySizePerCore  = KB(256);

/* -----Packet-level specification----- */
#define kRxPktHandler  kRxPktHandler_Empty

// client specific
#define EnableInflyMessageLimit true    // whether to enable infly message limit, if false, the client will send messages as fast as possible
static constexpr uint64_t kInflyMessageBudget = 8192;

/**
 * ----------------------OneStage modes----------------------
 */
/// Available types: kTxNICType, kTxDispatcherType, kTxApplicationType, kRxNICType, kRxDispatcherType, kRxApplicationType
// #define OneStage kRxApplicationType
/*!
 *  \note [xinyang] for app and dispatcher stage, max value is kWsQueueSize - 1, for nic stage, max 
                    value is kNumTxRingEntries
 *  \note [zhuobin] if this number exceed the size of mempool cache size, then the apply_bulk will
 *                  leak to the normal mempool mbufs, which might cause high cache miss rate
 */
#define FlowSize 256

/**
 * ----------------------General constants----------------------
 */ 

#define DPERF_LOG_LEVEL 3

static constexpr uint8_t kWorkspaceTypeNum = 3;
static constexpr uint8_t kInvaildWorkspaceType = std::pow(2, kWorkspaceTypeNum);
static constexpr uint8_t kWorkspaceMaxNum = 16;
static constexpr uint16_t kMaxBatchSize = 512;
static constexpr uint8_t kInvalidWsId = kWorkspaceMaxNum + 1;
static constexpr size_t  kWsQueueSize = 4096;    // Queue size must be power of two

/// Parameters for datapath pipeline
static constexpr uint8_t kMaxWorkloadNum = kWorkspaceMaxNum;
static constexpr uint8_t kInvalidWorkloadType = kMaxWorkloadNum + 1;

static constexpr uint8_t kTxNICType = 0;
static constexpr uint8_t kTxDispatcherType = 1;
static constexpr uint8_t kTxApplicationType = 2;
static constexpr uint8_t kRxNICType = 3;
static constexpr uint8_t kRxDispatcherType = 4;
static constexpr uint8_t kRxApplicationType = 5;

/**
 * ----------------------Simple methods----------------------
 */ 
static inline void rt_assert(bool condition, std::string throw_str, char *s) {
  if (unlikely(!condition)) {
    fprintf(stderr, "%s %s\n", throw_str.c_str(), s);
    exit(-1);
  }
}

static inline void rt_assert(bool condition, const char *throw_str) {
  if (unlikely(!condition)) {
    fprintf(stderr, "%s\n", throw_str);
    exit(-1);
  }
}

static inline void rt_assert(bool condition, std::string throw_str) {
  if (unlikely(!condition)) {
    fprintf(stderr, "%s\n", throw_str.c_str());
    exit(-1);
  }
}

static inline void rt_assert(bool condition) {
  if (unlikely(!condition)) {
    fprintf(stderr, "Error\n");
    assert(false);
    exit(-1);
  }
}

/// Check a condition at runtime. If the condition is false, print error message
/// and exit.
static inline void exit_assert(bool condition, std::string error_msg) {
  if (unlikely(!condition)) {
    fprintf(stderr, "%s. Exiting.\n", error_msg.c_str());
    fflush(stderr);
    exit(-1);
  }
}

/**
 * ----------------------Print related----------------------
 */ 
const std::string RESET = "\033[0m";
const std::string RED = "\033[31m";
const std::string GREEN = "\033[32m";
const std::string YELLOW = "\033[33m";
const std::string BLUE = "\033[34m";
}