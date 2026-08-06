/**
 * @file config.cc
 * @brief Parse Axio's legacy text configuration.
 */
#include "config.h"

#include "util/logger.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

namespace axio {

UserConfig::UserConfig(const std::string& filename) {
  printf("Load config file: %s\n", filename.c_str());
  this->_load(filename);
}

const std::vector<std::string>* UserConfig::value(const std::string& key) const {
  const auto value = this->config_values_.find(key);
  return value == this->config_values_.end() ? nullptr : &value->second;
}

void UserConfig::_load(const std::string& filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    std::cerr << "Failed to open config file: " << filename << std::endl;
    return;
  }

  std::string line;
  while (std::getline(file, line)) {
    const size_t first = line.find_first_not_of(' ');
    if (first == std::string::npos || line[first] == '#') {
      continue;
    }

    const std::vector<std::string> values = this->_split(line, ':');
    if (values.size() < 2) {
      continue;
    }

    const std::string key = this->_trim(values[0]);
    if (key == "workload") {
      std::vector<std::string> workload_values;
      for (size_t i = 1; i < values.size(); ++i) {
        workload_values.push_back(this->_trim(values[i]));
      }
      this->_configure_workload(workload_values);
      continue;
    }

    for (size_t i = 1; i < values.size(); ++i) {
      this->config_values_[key].push_back(this->_trim(values[i]));
    }
  }

  this->_configure_server();
}

std::vector<std::string> UserConfig::_split(const std::string& value, char delimiter) {
  std::vector<std::string> tokens;
  std::istringstream stream(value);
  std::string token;
  while (std::getline(stream, token, delimiter)) {
    tokens.push_back(token);
  }
  return tokens;
}

std::string UserConfig::_trim(const std::string& value) {
  const size_t first = value.find_first_not_of(' ');
  if (first == std::string::npos) {
    return {};
  }
  const size_t last = value.find_last_not_of(' ');
  return value.substr(first, last - first + 1);
}

void UserConfig::_configure_workload(const std::vector<std::string>& values) {
  uint8_t value_index = 0;
  uint8_t workload_type = kInvalidWorkloadType;

  for (const auto& value : values) {
    if (value_index == 0) {
      workload_type = std::stoi(value);
      rt_assert(workload_type < kInvalidWorkloadType, "Invalid workload type");
      value_index++;
      continue;
    }

    rt_assert(workload_type != kInvalidWorkloadType,
              "Please specify workload type first");
    if (value_index == 1) {
      for (const auto& phase : this->_split(value, ',')) {
        this->workloads_.pipeline_phases_[workload_type].push_back(phase);
      }
      value_index++;
      continue;
    }

    if (value_index == 2) {
      for (const auto& workspace_id_text : this->_split(value, ',')) {
        const uint8_t workspace_id = std::stoi(workspace_id_text);
        rt_assert(workspace_id < kInvalidWsId, "Invalid workspace id");
        this->workloads_.remote_dispatchers_[workload_type].push_back(workspace_id);
      }
      value_index++;
      continue;
    }

    const std::vector<std::string> group_values = this->_split(value, '|');
    uint8_t group_index = 0;
    for (const auto& workspace_value : group_values) {
      if (value_index == 3) {
        std::vector<uint8_t> application_workspaces;
        const size_t dash_position = workspace_value.find('-');
        if (dash_position != std::string::npos) {
          const std::vector<std::string> workspace_ids =
              this->_split(workspace_value, '-');
          if (workspace_ids.size() != 2) {
            AXIO_ERROR("Configuration for workload %s is not in the right format\n",
                       workspace_value.c_str());
            continue;
          }

          const uint8_t start_workspace_id = std::stoi(workspace_ids[0]);
          const uint8_t end_workspace_id = std::stoi(workspace_ids[1]);
          rt_assert(start_workspace_id < kInvalidWsId, "Invalid workspace id");
          rt_assert(end_workspace_id < kInvalidWsId, "Invalid workspace id");
          for (uint8_t workspace_id = start_workspace_id;
               workspace_id <= end_workspace_id; ++workspace_id) {
            application_workspaces.push_back(workspace_id);
            rt_assert(this->workloads_.workspace_workloads_.find(workspace_id) ==
                          this->workloads_.workspace_workloads_.end(),
                      "Workspace already assigned to a workload");
            this->workloads_.workspace_workloads_[workspace_id] = workload_type;
            this->workloads_.workspace_group_indices_[workspace_id] = group_index;
          }
        } else {
          const std::vector<std::string> workspace_ids =
              this->_split(workspace_value, ',');
          if (workspace_ids.empty()) {
            AXIO_ERROR("Configuration for workload %s is not in the right format\n",
                       workspace_value.c_str());
            continue;
          }

          for (const auto& workspace_id_text : workspace_ids) {
            const uint8_t workspace_id = std::stoi(workspace_id_text);
            rt_assert(workspace_id < kInvalidWsId, "Invalid workspace id");
            application_workspaces.push_back(workspace_id);
            rt_assert(this->workloads_.workspace_workloads_.find(workspace_id) ==
                          this->workloads_.workspace_workloads_.end(),
                      "Workspace already assigned to a workload");
            this->workloads_.workspace_workloads_[workspace_id] = workload_type;
            this->workloads_.workspace_group_indices_[workspace_id] = group_index;
          }
        }

        this->workloads_.application_workspaces_[workload_type].push_back(
            std::move(application_workspaces));
        group_index++;
      } else if (value_index == 4) {
        const uint8_t workspace_id = std::stoi(workspace_value);
        rt_assert(workspace_id < kInvalidWsId, "Invalid workspace id");
        this->workloads_.dispatchers_[workload_type].push_back(workspace_id);
      }
    }
    value_index++;
  }
}

void UserConfig::_configure_server() {
  for (const auto& entry : this->config_values_) {
    const auto& key = entry.first;
    const auto& value = entry.second[0];

    if (key == "numa") {
      this->server_.numa_node_ = std::stoi(value);
    } else if (key == "phy_port") {
      this->server_.physical_port_ = std::stoi(value);
    } else if (key == "iteration") {
      this->server_.iteration_count_ = std::stoi(value);
    } else if (key == "duration") {
      this->server_.duration_seconds_ = std::stoi(value);
    } else if (key == "local_ip") {
      std::strcpy(this->server_.local_ip_, value.c_str());
    } else if (key == "remote_ip") {
      std::strcpy(this->server_.remote_ip_, value.c_str());
    } else if (key == "local_mac") {
      const std::vector<std::string> octets = this->_split(value, '.');
      for (size_t i = 0; i < 6; ++i) {
        this->server_.local_mac_[i] = std::stoi(octets[i], nullptr, 16);
      }
    } else if (key == "remote_mac") {
      const std::vector<std::string> octets = this->_split(value, '.');
      for (size_t i = 0; i < 6; ++i) {
        this->server_.remote_mac_[i] = std::stoi(octets[i], nullptr, 16);
      }
    } else if (key == "device_pcie") {
      std::memcpy(this->server_.device_pcie_address_, value.c_str(), value.size());
      this->server_.device_pcie_address_[4] = ':';
      this->server_.device_pcie_address_[7] = ':';
      this->server_.device_pcie_address_[12] = '\0';
    } else if (key == "device_name") {
      std::memcpy(this->server_.device_name_, value.c_str(), value.size());
      this->server_.device_name_[value.size()] = '\0';
    } else if (key == "kAppCoreNum") {
      this->tunables_.app_core_count_ = std::stoi(value);
    } else if (key == "kDispQueueNum") {
      this->tunables_.dispatcher_queue_count_ = std::stoi(value);
    } else if (key == "kAppTxMsgBatchSize") {
      this->tunables_.app_tx_message_batch_size_ = std::stoi(value);
    } else if (key == "kAppRxMsgBatchSize") {
      this->tunables_.app_rx_message_batch_size_ = std::stoi(value);
    } else if (key == "kDispTxBatchSize") {
      this->tunables_.dispatcher_tx_batch_size_ = std::stoi(value);
    } else if (key == "kDispRxBatchSize") {
      this->tunables_.dispatcher_rx_batch_size_ = std::stoi(value);
    } else if (key == "kNICTxPostSize") {
      this->tunables_.nic_tx_post_size_ = std::stoi(value);
    } else if (key == "kNICRxPostSize") {
      this->tunables_.nic_rx_post_size_ = std::stoi(value);
    } else {
      AXIO_ERROR("Invalid server/tunable params config key %s\n", key.c_str());
    }
  }
}

void UserConfig::print() const {
  std::cout << "----------------------" << kAnsiYellow << "Basic Configuration"
            << kAnsiReset << "----------------------" << std::endl;
  printf("Node type: %s\n", AXIO_NODE_TYPE == AXIO_CLIENT ? "client" : "server");

  std::cout << "----------------------" << kAnsiYellow << "Workload Configuration"
            << kAnsiReset << "----------------------" << std::endl;
  for (const auto& workload : this->workloads_.application_workspaces_) {
    const uint8_t workload_type = workload.first;
    printf("Workload type %u:\n", workload_type);
    printf("    Pipeline phase: ");
    for (const auto& phase : this->workloads_.pipeline_phases_.at(workload_type)) {
      printf("%s ", phase.c_str());
    }
    printf("\n");
    for (size_t group_index = 0; group_index < workload.second.size(); ++group_index) {
      printf("    Workspace group %zu: App ", group_index);
      for (const uint8_t workspace_id : workload.second[group_index]) {
        printf("%u ", workspace_id);
      }
      printf("| Dispatcher %u\n",
             this->workloads_.dispatchers_.at(workload_type)[group_index]);
    }
  }

  std::cout << "----------------------" << kAnsiYellow << "Server Configuration"
            << kAnsiReset << "----------------------" << std::endl;
  printf("NUMA node: %u\n", this->server_.numa_node_);
  printf("Physical port: %u\n", this->server_.physical_port_);
  printf("Iteration: %u\n", this->server_.iteration_count_);
  printf("Duration: %u\n", this->server_.duration_seconds_);

  std::cout << "----------------------" << kAnsiYellow
            << "Current Tunable Params Configuration" << kAnsiReset
            << "----------------------" << std::endl;
  printf("App core number: %u\n", this->tunables_.app_core_count_);
  printf("Dispatcher queue number: %u\n", this->tunables_.dispatcher_queue_count_);
  printf("App tx batch size: %u\n", this->tunables_.app_tx_message_batch_size_);
  printf("App rx batch size: %u\n", this->tunables_.app_rx_message_batch_size_);
  printf("Dispatcher tx batch size: %u\n", this->tunables_.dispatcher_tx_batch_size_);
  printf("Dispatcher rx batch size: %u\n", this->tunables_.dispatcher_rx_batch_size_);
  printf("NIC tx post size: %u\n", this->tunables_.nic_tx_post_size_);
  printf("NIC rx post size: %u\n", this->tunables_.nic_rx_post_size_);

  std::cout << "----------------------" << kAnsiYellow << "End of Configuration"
            << kAnsiReset << "----------------------\n" << std::endl;
}

}  // namespace axio
