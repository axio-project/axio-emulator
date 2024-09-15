/**
 * @brief user defined packet handlers for emulation
 */
#include "roce_dispatcher.h"

namespace dperf {
  /**
   * @brief packet handler wrapper
   */
  template <pkt_handler_type_t handler>
  size_t RoceDispatcher::pkt_handler_server() {
    if constexpr (handler == kRxPktHandler_Empty) { return 0; }
    else {DPERF_ERROR("Invalid packet handler type!"); return 0;} 
  }

// force compile
template size_t RoceDispatcher::pkt_handler_server<kRxPktHandler>();
} // namespace dperf 