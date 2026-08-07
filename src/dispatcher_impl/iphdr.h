/*
 * Copyright (c) 2021-2022 Baidu.com, Inc. All Rights Reserved.
 * Copyright (c) 2022-2023 Jianzhang Peng. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Jianzhang Peng (pengjianzhang@baidu.com)
 *         Jianzhang Peng (pengjianzhang@gmail.com)
 */
#pragma once

#include <arpa/inet.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <netinet/ip.h>
#include <netinet/ip6.h>

namespace axio {

inline const uint16_t kIpv4DoNotFragment = htons(0x4000);

/*
 * The lower 32 bits represent an IPv6 address.
 * The IPv4 address is in the same position as the lower 32 bits of IPv6.
 * */
struct IpAddress {
  union {
    in6_addr ipv6_;
    struct {
      uint32_t padding_[3];
      uint32_t ipv4_;
    };
  };
};

static_assert(sizeof(IpAddress) == sizeof(in6_addr));
static_assert(offsetof(IpAddress, ipv4_) == 12);

#define AXIO_EXTRACT_IP_ADDRESS_LOW32(ip_header, source, destination) do { \
  const auto* ipv6_header =                                              \
      reinterpret_cast<const struct ip6_hdr*>(ip_header);                \
  if ((ip_header)->version == 4) {                                       \
    (source) = (ip_header)->saddr;                                       \
    (destination) = (ip_header)->daddr;                                  \
  } else {                                                               \
    (source) = ipv6_header->ip6_src.s6_addr32[3];                         \
    (destination) = ipv6_header->ip6_dst.s6_addr32[3];                    \
  }                                                                      \
} while (0)

inline void join_ip_address(const IpAddress* prefix, uint32_t last,
                            IpAddress* address) {
  address->ipv6_ = prefix->ipv6_;
  address->ipv4_ = last;
}

inline uint8_t last_ip_address_byte(const IpAddress& address) {
  return address.ipv6_.s6_addr[15];
}

inline bool ip_addresses_equal(const IpAddress* first,
                               const IpAddress* second) {
  return std::memcmp(first, second, sizeof(IpAddress)) == 0;
}

#if defined(__linux__)
inline void swap_ipv4_addresses(struct iphdr* header) {
  const uint32_t address = header->saddr;
  header->saddr = header->daddr;
  header->daddr = address;
}
#endif

inline void swap_ipv6_addresses(ip6_hdr* header) {
  const in6_addr address = header->ip6_src;
  header->ip6_src = header->ip6_dst;
  header->ip6_dst = address;
}

#define AXIO_IPV4_BYTES(address) \
    reinterpret_cast<const unsigned char*>(&(address))[0], \
    reinterpret_cast<const unsigned char*>(&(address))[1], \
    reinterpret_cast<const unsigned char*>(&(address))[2], \
    reinterpret_cast<const unsigned char*>(&(address))[3]
#define AXIO_IPV4_FORMAT "%u.%u.%u.%u"

#define AXIO_IPV6_WORDS(address) \
    ntohs(reinterpret_cast<const uint16_t*>(&(address))[0]), \
    ntohs(reinterpret_cast<const uint16_t*>(&(address))[1]), \
    ntohs(reinterpret_cast<const uint16_t*>(&(address))[2]), \
    ntohs(reinterpret_cast<const uint16_t*>(&(address))[3]), \
    ntohs(reinterpret_cast<const uint16_t*>(&(address))[4]), \
    ntohs(reinterpret_cast<const uint16_t*>(&(address))[5]), \
    ntohs(reinterpret_cast<const uint16_t*>(&(address))[6]), \
    ntohs(reinterpret_cast<const uint16_t*>(&(address))[7])
#define AXIO_IPV6_FORMAT "%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x"

int parse_ip_address(IpAddress* address, const char* input);
void increment_ip_address(IpAddress* address, uint32_t increment);
uint32_t parse_ipv4_host_order(const char* input);

}  // namespace axio
