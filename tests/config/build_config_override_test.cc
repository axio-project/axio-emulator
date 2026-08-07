#include "common.h"

#ifdef AXIO_EXPECT_ROCE_FALLBACK
static_assert(AXIO_NODE_TYPE == AXIO_CLIENT);
static_assert(AXIO_DPDK_MODE == 0);
static_assert(AXIO_ROCE_MODE == 1);
static_assert(AXIO_CONFIG_MEMPOOL_HANDLER ==
              AXIO_MEMPOOL_HANDLER_HUGE_ALLOC);
static_assert(AXIO_CONFIG_MEMPOOL_CACHE_SIZE == 0);
#elif defined(AXIO_EXPECT_OVERRIDE)
static_assert(AXIO_NODE_TYPE == AXIO_CLIENT);
static_assert(AXIO_DPDK_MODE == 0);
static_assert(AXIO_ROCE_MODE == 1);
static_assert(AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_UD);
static_assert(AXIO_RX_MESSAGE_HANDLER == axio::kMessageHandlerLatency);
static_assert(AXIO_RX_PACKET_HANDLER == axio::kPacketHandlerEcho);
static_assert(AXIO_APPLY_NEW_BUFFER == 1);
static_assert(axio::kAppReqPayloadSize == 86);
static_assert(axio::kAppRespPayloadSize == 86);
static_assert(axio::kAppTicksPerMsg == 7);
static_assert(AXIO_ENABLE_INFLIGHT_LIMIT == 0);
static_assert(axio::kInflightMessageBudget == 77);
static_assert(AXIO_CONFIG_RX_RING_ENTRIES == 1024);
static_assert(AXIO_CONFIG_TX_RING_ENTRIES == 512);
static_assert(AXIO_CONFIG_MEMPOOL_SIZE == 16384);
static_assert(AXIO_CONFIG_MTU == 4096);
static_assert(AXIO_CONFIG_MEMPOOL_HANDLER == 9);
static_assert(AXIO_CONFIG_MEMPOOL_CACHE_SIZE == 64);
#else
static_assert(AXIO_NODE_TYPE == AXIO_SERVER);
static_assert(AXIO_DPDK_MODE == 1);
static_assert(AXIO_ROCE_MODE == 0);
static_assert(AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_RC);
static_assert(AXIO_RX_MESSAGE_HANDLER == axio::kMessageHandlerThroughput);
static_assert(AXIO_RX_PACKET_HANDLER == axio::kPacketHandlerEmpty);
static_assert(AXIO_APPLY_NEW_BUFFER == 0);
static_assert(axio::kAppReqPayloadSize == 982);
static_assert(axio::kAppRespPayloadSize == 22);
static_assert(axio::kAppTicksPerMsg == 0);
static_assert(AXIO_ENABLE_INFLIGHT_LIMIT == 1);
static_assert(axio::kInflightMessageBudget == 1024);
static_assert(AXIO_CONFIG_RX_RING_ENTRIES == 2048);
static_assert(AXIO_CONFIG_TX_RING_ENTRIES == 2048);
static_assert(AXIO_CONFIG_MEMPOOL_SIZE == 8192);
static_assert(AXIO_CONFIG_MTU == 2048);
static_assert(AXIO_CONFIG_MEMPOOL_HANDLER == 0);
static_assert(AXIO_CONFIG_MEMPOOL_CACHE_SIZE == 0);
#endif

int main() { return 0; }
