#pragma once

#include "common.h"
#include "dispatcher_impl/ethhdr.h"
#include "dispatcher_impl/iphdr.h"
#include "ws_impl/ws_hdr.h"
#include <netinet/udp.h>

namespace dperf {
/// A class to hold a fixed-size buffer. The size of the buffer is read-only
/// after the Buffer is created.
class Buffer {
 public:
  static constexpr uint8_t kPOSTED = 0;
  static constexpr uint8_t kAPP_OWNED_BUF = 1;
  static constexpr uint8_t kFREE_BUF = 2;
  Buffer(uint8_t *buf, size_t class_size, uint32_t lkey)
      : buf_(buf), class_size_(class_size), lkey_(lkey) {}

  Buffer() {}

  /// Since \p Buffer does not allocate its own \p buf, do nothing here.
  ~Buffer() {}

  /// Return a string representation of this Buffer (excluding lkey)
  std::string to_string() const {
    std::ostringstream ret;
    ret << "[buf " << static_cast<void *>(buf_) << ", "
        << "class sz " << class_size_ << "]";
    return ret.str();
  }

  std::string buffer_print() {
    struct eth_hdr *eh = NULL;
    struct iphdr *iph = NULL;
    struct udphdr *uh = NULL;
    struct ws_hdr *wsh = NULL;
    char smac[64];
    char dmac[64];
    eh = reinterpret_cast<eth_hdr*>(get_eth());
    eth_addr_to_str(&eh->s_addr, smac);
    eth_addr_to_str(&eh->d_addr, dmac);

    char log[2048] = {0};
    if (eh->type == htons(ETHERTYPE_IP)) {
        iph = reinterpret_cast<iphdr*>(get_iph()); 
        uh = reinterpret_cast<udphdr*>(get_uh());
        wsh = reinterpret_cast<ws_hdr*>(get_ws_hdr());
        snprintf(log, sizeof(log), 
            "muf: %s -> %s " IPV4_FMT ":%u ->" IPV4_FMT ":%u proto %u ws_type: %u ws_seg: %lu payload_size: %lu\n",
            smac, dmac, 
            IPV4_STR(iph->saddr), ntohs(uh->source), 
            IPV4_STR(iph->daddr), ntohs(uh->dest), 
            iph->protocol, 
            wsh->workload_type_, 
            wsh->segment_num_, 
            strlen(reinterpret_cast<char*>(wsh) + sizeof(struct ws_hdr)));
    } else {
        snprintf(log, sizeof(log), "muf: %s -> %s type %x\n", 
            smac, dmac, ntohs(eh->type));
    }
    return std::string(log);
  }

  void set_lkey(uint32_t lkey) { lkey_ = lkey; }

  uint8_t* get_buf() { return buf_; }
  uint8_t* get_buf_offset(size_t offset) { return buf_ + offset; }
  uint8_t* get_ws_payload() { return buf_ + 14 + sizeof(struct iphdr) + sizeof(struct udphdr) + sizeof(struct ws_hdr);}
  uint8_t* get_ws_hdr() { return buf_ + 14 + sizeof(struct iphdr) + sizeof(struct udphdr); }
  uint8_t* get_uh() { return buf_ + 14 + sizeof(struct iphdr); }
  uint8_t* get_iph() { return buf_ + 14; }
  uint8_t* get_eth() { return buf_; }

  void set_length(uint32_t length) { length_ = length; }

  /// The backing memory of this Buffer. The Buffer is invalid if this is null.
  uint8_t *buf_;
  size_t class_size_;  ///< The allocator's class size
  uint32_t lkey_;      ///< The memory registration lkey
  uint32_t length_ = 0;    ///< The length of the buffer
  /// Using for RX
  Buffer *next_;       ///< Next Buffer
  uint8_t state_ = kFREE_BUF;  /// 0: owned by nic; 1: owned by app; 2: free, waiting for post_recv
};

}  // namespace dperf
