/**
 * @brief user defined packet handlers for emulation
 */
#include "roce_dispatcher.h"

namespace axio {
  /**
   * @brief packet handler wrapper
   */
  template <PacketHandlerType handler>
  size_t RoceDispatcher::handle_server_packets() {
    if constexpr (handler == kPacketHandlerEmpty) { return 0; }
    else {AXIO_ERROR("Invalid packet handler type!"); return 0;}
  }

// force compile
template size_t RoceDispatcher::handle_server_packets<AXIO_RX_PACKET_HANDLER>();
} // namespace axio
