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

#include "iphdr.h"

#include "common.h"

namespace axio {
int parse_ip_address(IpAddress* address, const char* input) {
  int address_family = -1;

  std::memset(address, 0, sizeof(IpAddress));
  int result;
  if (std::strchr(input, ':')) {
    address_family = AF_INET6;
    result = inet_pton(address_family, input, &address->ipv6_);
  } else {
    address_family = AF_INET;
    result = inet_pton(address_family, input, &address->ipv4_);
  }

  return result == 1 ? address_family : -1;
}

void increment_ip_address(IpAddress* address, uint32_t increment) {
  const uint32_t incremented_address = ntohl(address->ipv4_) + increment;
  address->ipv4_ = htonl(incremented_address);
}

/// Get the host-byte-order IPv4 address from a human-readable IP string
uint32_t parse_ipv4_host_order(const char* input) {
  uint32_t addr;
  int ret = inet_pton(AF_INET, input, &addr);  // addr is in network-byte order
  rt_assert(ret == 1, "inet_pton() failed for " + std::string(input));
  return ntohl(addr);
}

}  // namespace axio
