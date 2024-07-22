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

namespace dperf {
int ipaddr_init(ipaddr_t *ip, const char *str)
{
    int ret = 0;
    int af = -1;

    memset(ip, 0, sizeof(ipaddr_t));
    if (strchr(str, ':')) {
        af = AF_INET6;
        ret = inet_pton(af, str, &ip->in6);
    } else {
        af = AF_INET;
        ret = inet_pton(af, str, &ip->ip);
    }

    if (ret == 1) {
        return af;
    }

    return -1;
}

void ipaddr_inc(ipaddr_t *ip, uint32_t n)
{
    uint32_t addr = 0;

    addr = ntohl(ip->ip) + n;
    ip->ip = htonl(addr);
}

/// Get the host-byte-order IPv4 address from a human-readable IP string
uint32_t ipv4_from_str(const char* ip) {
  uint32_t addr;
  int ret = inet_pton(AF_INET, ip, &addr);  // addr is in network-byte order
  rt_assert(ret == 1, "inet_pton() failed for " + std::string(ip));
  return ntohl(addr);
}

}