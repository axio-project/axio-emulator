/**
 * @brief user defined packet handlers for emulation
 */
#include "roce_dispatcher.h"

namespace axio {
  /**
   * @brief packet handler wrapper
   */
  template <PacketHandlerType handler>
  size_t RoceDispatcher::pkt_handler_server() {
    if constexpr (handler == kPacketHandlerEmpty) { return 0; }
    else {AXIO_ERROR("Invalid packet handler type!"); return 0;}
  }

// force compile
template size_t RoceDispatcher::pkt_handler_server<AXIO_RX_PACKET_HANDLER>();
} // namespace axio
