/**
 * @file config.h
 * @brief Load and store configuration parameters.
 */
#pragma once

#include "common.h"

#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace axio {

class UserConfig {
 public:
  struct WorkloadsConfig {
    std::map<uint8_t, std::vector<std::string>> pipeline_phases_;
    std::map<uint8_t, std::vector<std::vector<uint8_t>>> application_workspaces_;
    std::map<uint8_t, std::vector<uint8_t>> dispatchers_;
    std::map<uint8_t, std::vector<uint8_t>> remote_dispatchers_;
    std::map<uint8_t, uint8_t> workspace_workloads_;
    std::map<uint8_t, uint8_t> workspace_group_indices_;

    size_t size() const { return this->pipeline_phases_.size(); }

    uint8_t type_at(size_t workload_index) const {
      auto workload = this->pipeline_phases_.begin();
      std::advance(workload, workload_index);
      return workload->first;
    }
  };

  struct ServerConfig {
    uint8_t numa_node_ = 0;
    uint8_t physical_port_ = 0;
    uint8_t iteration_count_ = 0;
    uint8_t duration_seconds_ = 0;
    char local_ip_[16] = {};
    char remote_ip_[16] = {};
    uint8_t local_mac_[6] = {};
    uint8_t remote_mac_[6] = {};
    char device_pcie_address_[13] = {};
    char device_name_[32] = {};
  };

  struct TunableParams {
    uint8_t app_core_count_ = 16;
    uint8_t dispatcher_queue_count_ = 16;
    uint16_t app_tx_message_batch_size_ = 32;
    uint16_t app_rx_message_batch_size_ = 32;
    uint16_t dispatcher_tx_batch_size_ = 32;
    uint16_t dispatcher_rx_batch_size_ = 32;
    uint16_t nic_tx_post_size_ = 32;
    uint16_t nic_rx_post_size_ = 32;
  };

  explicit UserConfig(const std::string& filename);

  const std::vector<std::string>* value(const std::string& key) const;
  const WorkloadsConfig& workloads() const { return this->workloads_; }
  const ServerConfig& server() const { return this->server_; }
  const TunableParams& tunables() const { return this->tunables_; }

  uint8_t numa_node() const { return this->server_.numa_node_; }
  uint8_t physical_port() const { return this->server_.physical_port_; }
  uint8_t iteration_count() const { return this->server_.iteration_count_; }
  uint8_t duration_seconds() const { return this->server_.duration_seconds_; }

  void print() const;

 private:
  std::map<std::string, std::vector<std::string>> config_values_;
  WorkloadsConfig workloads_;
  ServerConfig server_;
  TunableParams tunables_;

  void _load(const std::string& filename);
  static std::vector<std::string> _split(const std::string& value, char delimiter);
  static std::string _trim(const std::string& value);
  void _configure_workload(const std::vector<std::string>& values);
  void _configure_server();
};

}  // namespace axio
