#include "dpdk_externs.h"

namespace axio {

std::mutex g_dpdk_lock;
bool g_dpdk_initialized;
bool g_port_initialized[RTE_MAX_ETHPORTS];
DpdkDispatcher::ownership_memzone_t *g_memzone;

}  // namespace axio
