/**
 * @file config.cc
 * @brief Adapt typed Axio configuration to emulator runtime structures.
 */
#include "config.h"

#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace axio {
namespace {

template <typename TDestination>
TDestination narrow_unsigned(uint32_t value, const char* key) {
  if (value > std::numeric_limits<TDestination>::max()) {
    throw std::out_of_range(std::string(key) + " exceeds runtime storage");
  }
  return static_cast<TDestination>(value);
}

template <size_t Size>
void copy_string(char (&destination)[Size], const std::string& source,
                 const char* key) {
  if (source.size() >= Size) {
    throw std::out_of_range(std::string(key) + " exceeds runtime storage");
  }
  std::memcpy(destination, source.c_str(), source.size() + 1);
}

void parse_mac(const std::string& source, uint8_t* destination,
               const char* key) {
  std::istringstream input(source);
  std::string octet;
  for (size_t index = 0; index < 6; ++index) {
    if (!std::getline(input, octet, ':') || octet.size() != 2) {
      throw std::invalid_argument(std::string(key) + " is not a canonical MAC");
    }
    destination[index] = static_cast<uint8_t>(std::stoul(octet, nullptr, 16));
  }
  if (std::getline(input, octet, ':')) {
    throw std::invalid_argument(std::string(key) + " is not a canonical MAC");
  }
}

const char* legacy_phase_name(config::PipelinePhase phase) {
  switch (phase) {
    case config::PipelinePhase::kApplicationTx: return "TxApplication";
    case config::PipelinePhase::kDispatcherTx: return "TxDispatcher";
    case config::PipelinePhase::kNicTx: return "TxNIC";
    case config::PipelinePhase::kNicRx: return "RXNIC";
    case config::PipelinePhase::kDispatcherRx: return "RXDispatcher";
    case config::PipelinePhase::kApplicationRx: return "RxApplication";
  }
  throw std::invalid_argument("unsupported pipeline phase");
}

}  // namespace

config::AxioConfig compiled_build_config() {
  config::AxioConfig compiled;
  compiled.schema_version = AXIO_CONFIG_SCHEMA_VERSION;
  compiled.deployment.role = AXIO_NODE_TYPE == AXIO_CLIENT
                                 ? config::Role::kClient
                                 : config::Role::kServer;
  compiled.network.backend = AXIO_DPDK_MODE ? config::Backend::kDpdk
                                            : config::Backend::kRoce;
  compiled.network.roce_transport =
      AXIO_ROCE_TRANSPORT_TYPE == AXIO_ROCE_RC
          ? config::RoceTransport::kRc
          : config::RoceTransport::kUd;
  compiled.network.rx_ring_entries = AXIO_CONFIG_RX_RING_ENTRIES;
  compiled.network.tx_ring_entries = AXIO_CONFIG_TX_RING_ENTRIES;
  compiled.handler.message_handler =
      static_cast<config::MessageHandler>(AXIO_CONFIG_MESSAGE_HANDLER);
  compiled.handler.packet_handler =
      static_cast<config::PacketHandler>(AXIO_CONFIG_PACKET_HANDLER);
  compiled.handler.apply_new_mbuf = AXIO_CONFIG_APPLY_NEW_MBUF != 0;
  compiled.handler.request_payload_bytes = AXIO_CONFIG_REQUEST_PAYLOAD_BYTES;
  compiled.handler.response_payload_bytes = AXIO_CONFIG_RESPONSE_PAYLOAD_BYTES;
  compiled.handler.app_ticks_per_message = AXIO_CONFIG_APP_TICKS_PER_MESSAGE;
  compiled.knobs.build.inflight_limit_enabled =
      AXIO_CONFIG_INFLIGHT_LIMIT_ENABLED != 0;
  compiled.knobs.build.inflight_messages = AXIO_CONFIG_INFLIGHT_MESSAGES;
  compiled.knobs.build.mtu = AXIO_CONFIG_MTU;
  compiled.knobs.build.mempool_handler =
      static_cast<config::MempoolHandler>(AXIO_CONFIG_MEMPOOL_HANDLER);
  compiled.other.mempool_size = AXIO_CONFIG_MEMPOOL_SIZE;
  compiled.other.mempool_cache_size = AXIO_CONFIG_MEMPOOL_CACHE_SIZE;
  return compiled;
}

BuildFingerprintComparison compare_build_fingerprint(
    const config::AxioConfig& runtime_config) {
#ifdef AXIO_CONFIG_BUILD_FINGERPRINT
  const std::string embedded = AXIO_CONFIG_BUILD_FINGERPRINT;
#else
  const std::string embedded =
      config::build_fingerprint(compiled_build_config());
#endif
  return {embedded, config::build_fingerprint(runtime_config)};
}

UserConfig::UserConfig(const config::AxioConfig& config)
    : topology_(config::ValidatedTopology::from_config(config)) {
  this->server_.numa_node_ =
      narrow_unsigned<uint8_t>(config.deployment.numa_node,
                               "deployment.numa_node");
  this->server_.physical_port_ = narrow_unsigned<uint8_t>(
      config.network.physical_port, "network.physical_port");
  this->server_.iteration_count_ = narrow_unsigned<uint8_t>(
      config.other.iterations, "other.iterations");
  this->server_.duration_seconds_ = narrow_unsigned<uint8_t>(
      config.other.window_seconds, "other.window_seconds");
  copy_string(this->server_.local_ip_, config.network.local_ip,
              "network.local_ip");
  copy_string(this->server_.remote_ip_, config.network.remote_ip,
              "network.remote_ip");
  parse_mac(config.network.local_mac, this->server_.local_mac_,
            "network.local_mac");
  parse_mac(config.network.remote_mac, this->server_.remote_mac_,
            "network.remote_mac");
  copy_string(this->server_.device_pcie_address_, config.network.device_pcie,
              "network.device_pcie");
  copy_string(this->server_.device_name_, config.network.device_name,
              "network.device_name");

  this->tunables_.app_tx_message_batch_size_ = narrow_unsigned<uint16_t>(
      config.knobs.runtime.app_tx_batch_size,
      "knobs.runtime.app_tx_batch_size");
  this->tunables_.app_rx_message_batch_size_ = narrow_unsigned<uint16_t>(
      config.knobs.runtime.app_rx_batch_size,
      "knobs.runtime.app_rx_batch_size");
  this->tunables_.dispatcher_tx_batch_size_ = narrow_unsigned<uint16_t>(
      config.knobs.runtime.dispatcher_tx_batch_size,
      "knobs.runtime.dispatcher_tx_batch_size");
  this->tunables_.dispatcher_rx_batch_size_ = narrow_unsigned<uint16_t>(
      config.knobs.runtime.dispatcher_rx_batch_size,
      "knobs.runtime.dispatcher_rx_batch_size");
  this->tunables_.nic_tx_post_size_ = narrow_unsigned<uint16_t>(
      config.knobs.runtime.nic_tx_post_size,
      "knobs.runtime.nic_tx_post_size");
  this->tunables_.nic_rx_post_size_ = narrow_unsigned<uint16_t>(
      config.knobs.runtime.nic_rx_post_size,
      "knobs.runtime.nic_rx_post_size");
  this->tunables_.app_core_count_ = narrow_unsigned<uint8_t>(
      this->topology_.application_core_count(),
      "knobs.runtime.application_core_count");
  this->tunables_.dispatcher_queue_count_ = narrow_unsigned<uint8_t>(
      this->topology_.dispatcher_queue_count(),
      "knobs.runtime.dispatcher_queue_count");

  for (const uint32_t workload_value : this->topology_.active_workload_ids()) {
    const config::ValidatedWorkload& workload =
        this->topology_.workload(workload_value);
    const uint8_t workload_id =
        narrow_unsigned<uint8_t>(workload.id, "workloads.id");
    for (const config::PipelinePhase phase : workload.pipeline) {
      this->workloads_.pipeline_phases_[workload_id].push_back(
          legacy_phase_name(phase));
    }
    for (const config::WorkspaceId remote_dispatcher :
         workload.remote_dispatchers) {
      this->workloads_.remote_dispatchers_[workload_id].push_back(
          narrow_unsigned<uint8_t>(remote_dispatcher.value(),
                                   "workloads.remote_dispatchers"));
    }
    for (size_t group_index = 0; group_index < workload.groups.size();
         ++group_index) {
      const config::ValidatedGroup& group = workload.groups[group_index];
      const uint8_t dispatcher = narrow_unsigned<uint8_t>(
          group.dispatcher.value(), "workloads.groups.dispatcher");
      this->workloads_.dispatchers_[workload_id].push_back(dispatcher);
      std::vector<uint8_t> applications;
      for (const config::WorkspaceId application_id : group.applications) {
        const uint8_t application = narrow_unsigned<uint8_t>(
            application_id.value(), "workloads.groups.applications");
        applications.push_back(application);
        this->workloads_.workspace_workloads_[application] = workload_id;
        this->workloads_.workspace_group_indices_[application] =
            narrow_unsigned<uint8_t>(group_index, "workloads.groups");
      }
      this->workloads_.application_workspaces_[workload_id].push_back(
          std::move(applications));
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
