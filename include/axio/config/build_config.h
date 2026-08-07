/**
 * @file build_config.h
 * @brief Canonical names and fingerprinting for build-time Axio knobs.
 */
#pragma once

#include "axio/config/config_types.h"

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace axio::config {

inline std::string_view to_string(Role value) {
  return value == Role::kClient ? "client" : "server";
}

inline std::string_view to_string(DeploymentTransport value) {
  return value == DeploymentTransport::kLocal ? "local" : "ssh";
}

inline std::string_view to_string(Backend value) {
  return value == Backend::kDpdk ? "dpdk" : "roce";
}

inline std::string_view to_string(RoceTransport value) {
  return value == RoceTransport::kRc ? "rc" : "ud";
}

inline std::string_view to_string(MempoolHandler value) {
  switch (value) {
    case MempoolHandler::kRingMpMc: return "ring_mp_mc";
    case MempoolHandler::kRingSpSc: return "ring_sp_sc";
    case MempoolHandler::kRingMpSc: return "ring_mp_sc";
    case MempoolHandler::kRingSpMc: return "ring_sp_mc";
    case MempoolHandler::kRingMtRts: return "ring_mt_rts";
    case MempoolHandler::kRingMtHts: return "ring_mt_hts";
    case MempoolHandler::kStack: return "stack";
    case MempoolHandler::kLfStack: return "lf_stack";
    case MempoolHandler::kBucket: return "bucket";
    case MempoolHandler::kHugeAlloc: return "huge_alloc";
  }
  return "unknown";
}

inline std::string_view to_string(MessageHandler value) {
  switch (value) {
    case MessageHandler::kEmpty: return "empty";
    case MessageHandler::kThroughput: return "t_app";
    case MessageHandler::kLatency: return "l_app";
    case MessageHandler::kMemory: return "m_app";
    case MessageHandler::kFileWrite: return "file_write";
    case MessageHandler::kFileRead: return "file_read";
    case MessageHandler::kKeyValue: return "key_value";
  }
  return "unknown";
}

inline std::string_view to_string(PacketHandler value) {
  return value == PacketHandler::kEmpty ? "empty" : "echo";
}

inline std::string_view to_string(PipelinePhase value) {
  switch (value) {
    case PipelinePhase::kApplicationTx: return "app_tx";
    case PipelinePhase::kDispatcherTx: return "dispatcher_tx";
    case PipelinePhase::kNicTx: return "nic_tx";
    case PipelinePhase::kNicRx: return "nic_rx";
    case PipelinePhase::kDispatcherRx: return "dispatcher_rx";
    case PipelinePhase::kApplicationRx: return "app_rx";
  }
  return "unknown";
}

inline std::string canonical_build_config(const AxioConfig& config) {
  std::ostringstream output;
  output << "schema_version=" << config.schema_version
         << ";role=" << to_string(config.deployment.role)
         << ";backend=" << to_string(config.network.backend)
         << ";roce_transport=" << to_string(config.network.roce_transport)
         << ";rx_ring_entries=" << config.network.rx_ring_entries
         << ";tx_ring_entries=" << config.network.tx_ring_entries
         << ";message_handler=" << to_string(config.handler.message_handler)
         << ";packet_handler=" << to_string(config.handler.packet_handler)
         << ";apply_new_mbuf=" << (config.handler.apply_new_mbuf ? 1 : 0)
         << ";request_payload_bytes=" << config.handler.request_payload_bytes
         << ";response_payload_bytes=" << config.handler.response_payload_bytes
         << ";app_ticks_per_message=" << config.handler.app_ticks_per_message
         << ";inflight_limit_enabled="
         << (config.knobs.build.inflight_limit_enabled ? 1 : 0)
         << ";inflight_messages=" << config.knobs.build.inflight_messages
         << ";mtu=" << config.knobs.build.mtu
         << ";mempool_handler="
         << to_string(config.knobs.build.mempool_handler)
         << ";mempool_size=" << config.other.mempool_size
         << ";mempool_cache_size=" << config.other.mempool_cache_size;
  return output.str();
}

inline std::string fingerprint_text(std::string_view input) {
  constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
  constexpr uint64_t kFnvPrime = 1099511628211ULL;
  uint64_t hash = kFnvOffset;
  for (const unsigned char byte : input) {
    hash ^= byte;
    hash *= kFnvPrime;
  }
  std::ostringstream output;
  output << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16)
         << hash;
  return output.str();
}

inline std::string build_fingerprint(const AxioConfig& config) {
  return fingerprint_text(canonical_build_config(config));
}

}  // namespace axio::config
