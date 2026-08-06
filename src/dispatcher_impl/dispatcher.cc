/**
 * @file dispatcher.cc
 * @brief Common initialization for dispatcher backends.
 */
#include "dispatcher.h"

namespace axio {

Dispatcher::Dispatcher(DispatcherType dispatcher_type, uint8_t ws_id, 
                       uint8_t phy_port, size_t numa_node,
                       UserConfig* user_config)
    : dispatcher_type_(dispatcher_type),
      physical_port_(phy_port),
      numa_node_(numa_node) {
  AXIO_UNUSED(ws_id);

  // Init and check tunable parameters
  const auto& tunables = user_config->tunables();
  rt_assert(tunables.dispatcher_queue_count_ <= kMaxQueuesPerPort, "Dispatcher queue number is too large");
  this->tx_batch_size_ = tunables.dispatcher_tx_batch_size_;
  rt_assert(this->tx_batch_size_ <= kMaxBatchSize,
            "Dispatcher TX batch size is too large");
  this->rx_batch_size_ = tunables.dispatcher_rx_batch_size_;
  rt_assert(this->rx_batch_size_ <= kMaxBatchSize,
            "Dispatcher RX batch size is too large");
  this->nic_tx_post_size_ = tunables.nic_tx_post_size_;
  rt_assert(this->nic_tx_post_size_ <= kMaxBatchSize,
            "NIC TX post size is too large");
  this->nic_rx_post_size_ = tunables.nic_rx_post_size_;
  rt_assert(this->nic_rx_post_size_ <= kMaxBatchSize,
            "NIC RX post size is too large");

  // Init ip and mac
  const auto& server = user_config->server();
  this->local_ip_ = server.local_ip_;
  this->remote_ip_ = server.remote_ip_;
  memcpy(this->local_mac_.bytes, server.local_mac_, 6);
  memcpy(this->remote_mac_.bytes, server.remote_mac_, 6);
}

Dispatcher::~Dispatcher() {}

}  // namespace axio
