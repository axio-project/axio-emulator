/**
 * @file dispatcher.cc
 * @brief General definitions for all dispatcher types. A Dispather instance encapsulate driver codes.
 */
#include "dispatcher.h"

namespace axio {

Dispatcher::Dispatcher(DispatcherType dispatcher_type, uint8_t ws_id, 
                            uint8_t phy_port, size_t numa_node, UserConfig *user_config)
    : dispatcher_type_(dispatcher_type),
      phy_port_(phy_port),
      numa_node_(numa_node) {
  // Init and check tunable parameters
  const auto& tunables = user_config->tunables();
  rt_assert(tunables.dispatcher_queue_count_ <= kMaxQueuesPerPort, "Dispatcher queue number is too large");
  dispatcher_tx_batch_size_ = tunables.dispatcher_tx_batch_size_;
  rt_assert(dispatcher_tx_batch_size_ <= kMaxBatchSize, "Dispatcher TX batch size is too large");
  dispatcher_rx_batch_size_ = tunables.dispatcher_rx_batch_size_;
  rt_assert(dispatcher_rx_batch_size_ <= kMaxBatchSize, "Dispatcher RX batch size is too large");
  nic_tx_post_size_ = tunables.nic_tx_post_size_;
  rt_assert(nic_tx_post_size_ <= kMaxBatchSize, "NIC TX post size is too large");
  nic_rx_post_size_ = tunables.nic_rx_post_size_;
  rt_assert(nic_rx_post_size_ <= kMaxBatchSize, "NIC RX post size is too large");
  // Init ip and mac
  const auto& server = user_config->server();
  kLocalIpStr = server.local_ip_;
  kRemoteIpStr = server.remote_ip_;
  memcpy(kLocalMac.bytes, server.local_mac_, 6);
  memcpy(kRemoteMac.bytes, server.remote_mac_, 6);
}

Dispatcher::~Dispatcher() {}

}
