#pragma once

#include "common.h"
#include "dispatcher_impl/iphdr.h"
#include "ws_impl/ws_hdr.h"

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
  static constexpr size_t kEthernetHeaderBytes = 14;

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
    auto* workspace = reinterpret_cast<ws_hdr*>(this->workspace_header());

    char log[2048] = {0};
    snprintf(
        log, sizeof(log),
        "buffer: %u -> %u, ws_type: %u, ws_seg: %lu, payload_size: %lu\n",
        ntohs(udp->source), ntohs(udp->dest), workspace->workload_type_,
        workspace->segment_num_,
        strlen(reinterpret_cast<char*>(workspace) + sizeof(ws_hdr)));
    return std::string(log);
  }

  void set_lkey(uint32_t local_key) { this->lkey_ = local_key; }
  void set_length(uint32_t length) { this->length_ = length; }

  uint8_t* data() { return this->buf_; }
  uint8_t* data_at(size_t offset) { return this->buf_ + offset; }
  uint8_t* workspace_payload() {
    return this->buf_ + kEthernetHeaderBytes + sizeof(iphdr) +
           sizeof(udphdr) + sizeof(ws_hdr);
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
};

}  // namespace axio
