#pragma once

#include "common.h"
#include "dispatcher_impl/ethhdr.h"
#include "dispatcher_impl/iphdr.h"
#include "ws_impl/workspace_header.h"

#include <netinet/udp.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

namespace axio {

/**
 * Passive hot-path descriptor for a fixed-size RoCE buffer.
 *
 * Fields remain public to preserve the existing layout and direct datapath
 * access. Buffer does not own the memory referenced by buf_.
 */
struct Buffer {
  static constexpr uint8_t kPosted = 0;
  static constexpr uint8_t kApplicationOwned = 1;
  static constexpr uint8_t kFree = 2;
  static constexpr size_t kEthernetHeaderBytes = sizeof(EthernetHeader);

  Buffer(uint8_t* buffer, size_t class_size, uint32_t local_key)
      : buf_(buffer), class_size_(class_size), lkey_(local_key) {}

  Buffer() = default;
  ~Buffer() = default;

  std::string to_string() const {
    std::ostringstream result;
    result << "[buf " << static_cast<void*>(this->buf_) << ", "
           << "class sz " << this->class_size_ << "]";
    return result.str();
  }

  std::string debug_string() {
    auto* udp = reinterpret_cast<udphdr*>(this->udp_header());
    auto* workspace = reinterpret_cast<WorkspaceHeader*>(this->workspace_header());

    char log[2048] = {0};
    snprintf(
        log, sizeof(log),
        "buffer: %u -> %u, ws_type: %u, ws_seg: %lu, payload_size: %lu\n",
        ntohs(udp->source), ntohs(udp->dest), workspace->workload_type_,
        workspace->segment_num_,
        strlen(reinterpret_cast<char*>(workspace) + sizeof(WorkspaceHeader)));
    return std::string(log);
  }

  void set_lkey(uint32_t local_key) { this->lkey_ = local_key; }
  void set_length(uint32_t length) { this->length_ = length; }

  bool try_acquire() {
    uint8_t expected = kFree;
    return __atomic_compare_exchange_n(
        &this->state_, &expected, kApplicationOwned, false,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
  }

  bool try_acquire_local() {
    if (this->state_ != kFree) return false;
    this->state_ = kApplicationOwned;
    return true;
  }

  uint8_t state() const {
    return __atomic_load_n(&this->state_, __ATOMIC_ACQUIRE);
  }

  void mark_application_owned() {
    __atomic_store_n(&this->state_, kApplicationOwned, __ATOMIC_RELEASE);
  }

  void mark_posted() {
    __atomic_store_n(&this->state_, kPosted, __ATOMIC_RELEASE);
  }

  void mark_free() {
    this->length_ = 0;
    __atomic_store_n(&this->state_, kFree, __ATOMIC_RELEASE);
  }

  void mark_free_local() {
    this->length_ = 0;
    this->state_ = kFree;
  }

  uint8_t* data() { return this->buf_; }
  uint8_t* data_at(size_t offset) { return this->buf_ + offset; }
  uint8_t* workspace_payload() {
    return this->buf_ + kEthernetHeaderBytes + sizeof(iphdr) +
           sizeof(udphdr) + sizeof(WorkspaceHeader);
  }
  uint8_t* workspace_header() {
    return this->buf_ + kEthernetHeaderBytes + sizeof(iphdr) + sizeof(udphdr);
  }
  uint8_t* udp_header() {
    return this->buf_ + kEthernetHeaderBytes + sizeof(iphdr);
  }
  uint8_t* ip_header() { return this->buf_ + kEthernetHeaderBytes; }

  uint8_t* buf_;
  size_t class_size_;
  uint32_t lkey_;
  uint32_t length_ = 0;
  Buffer* next_;
  uint8_t state_ = kFree;
  bool reusable_ = false;
};

}  // namespace axio
