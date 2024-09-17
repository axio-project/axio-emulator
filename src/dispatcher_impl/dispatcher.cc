/**
 * @file dispatcher.cc
 * @brief General definitions for all dispatcher types. A Dispather instance encapsulate driver codes.
 */
#include "dispatcher.h"

namespace dperf {

Dispatcher::Dispatcher(DispatcherType dispatcher_type, uint8_t ws_id, 
                            uint8_t phy_port, size_t numa_node, UserConfig *user_config)
    : dispatcher_type_(dispatcher_type),
      phy_port_(phy_port),
      numa_node_(numa_node) {
  // Init and check tunable parameters
  rt_assert(user_config->tune_params_ != nullptr, "Tunable parameters are not loaded");
  kDispTxBatchSize = user_config->tune_params_->kDispTxBatchSize;
  rt_assert(kDispTxBatchSize <= kMaxBatchSize, "Dispatcher TX batch size is too large");
  kDispRxBatchSize = user_config->tune_params_->kDispRxBatchSize;
  rt_assert(kDispRxBatchSize <= kMaxBatchSize, "Dispatcher RX batch size is too large");
  kNICTxPostSize = user_config->tune_params_->kNICTxPostSize;
  rt_assert(kNICTxPostSize <= kMaxBatchSize, "NIC TX post size is too large");
  kNICRxPostSize = user_config->tune_params_->kNICRxPostSize;
  rt_assert(kNICRxPostSize <= kMaxBatchSize, "NIC RX post size is too large");
}

Dispatcher::~Dispatcher() {}

}