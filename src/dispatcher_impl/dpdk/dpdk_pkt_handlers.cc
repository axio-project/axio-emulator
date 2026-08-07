/**
 * @brief user defined packet handlers for emulation
 */
#include "dpdk_dispatcher.h"

namespace axio {
/**
 * @brief Echo packet-handler kernel.
 */
size_t DpdkDispatcher::_handle_echo() {
  size_t pre_dispatch_total = 0;
  rte_mbuf* buffer;
  EthernetHeader* ethernet_header = nullptr;
  iphdr* ip_header = nullptr;

  uint8_t temporary_mac[kEthernetAddressLength] = {0};
  uint32_t temporary_ip = 0;

  size_t remaining_tx_capacity =
      (kNumTxRingEntries - this->tx_queue_index_ > this->rx_queue_index_)
          ? this->rx_queue_index_
          : kNumTxRingEntries - this->tx_queue_index_;
  for (size_t i = 0; i < remaining_tx_capacity; i++) {
    buffer = this->rx_queue_[i];
    ethernet_header = AXIO_MBUF_ETH_HEADER(buffer);
    ip_header = AXIO_MBUF_IP_HEADER(buffer);

    temporary_ip = ip_header->daddr;
    ip_header->daddr = ip_header->saddr;
    ip_header->saddr = temporary_ip;

    rte_memcpy(temporary_mac, ethernet_header->destination_.bytes_, kEthernetAddressLength);
    rte_memcpy(ethernet_header->destination_.bytes_, ethernet_header->source_.bytes_,
               kEthernetAddressLength);
    rte_memcpy(ethernet_header->source_.bytes_, temporary_mac, kEthernetAddressLength);

    this->tx_queue_[this->tx_queue_index_] = buffer;
    this->tx_queue_index_++;

    pre_dispatch_total++;
  }
  for (size_t i = pre_dispatch_total; i < this->rx_queue_index_; i++) {
    rte_pktmbuf_free(this->rx_queue_[i]);
  }
  this->rx_queue_index_ = 0;
  return pre_dispatch_total;
}

template <PacketHandlerType handler>
size_t DpdkDispatcher::handle_server_packets() {
  if constexpr (handler == kPacketHandlerEmpty) {
    return 0;
  } else if (handler == kPacketHandlerEcho) {
    return this->_handle_echo();
  } else {
    AXIO_ERROR("Invalid packet handler type!");
    return 0;
  }
}

// force compile
template size_t DpdkDispatcher::handle_server_packets<AXIO_RX_PACKET_HANDLER>();
}  // namespace axio
