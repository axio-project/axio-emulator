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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>

namespace dperf {

#define ETH_P_ARP     0x0806 // arp protocol code in eth header 
#define ETH_P_IP      0x0800 // ip protocol code in eth header 
#define ETH_ALEN      6      // eth adress length
#define ARPHRD_ETHER  1  // ethernet hardware type code in arp header
#define ARPOP_REQUEST 1
#define ARPOP_REPLY   2

struct arp_hdr_t {
  uint16_t  arp_hrd;		/* Format of hardware address.  */
  uint16_t  arp_pro;		/* Format of protocol address.  */
  uint8_t	arp_hln;		/* Length of hardware address.  */
  uint8_t	arp_pln;		/* Length of protocol address.  */
  uint16_t  arp_op;			/* ARP opcode (command).  */
  uint8_t	arp_sha[ETH_ALEN];	/* sender hardware address */
  uint32_t	arp_spa;		/* sender protocol address */
  uint8_t	arp_tha[ETH_ALEN];	/* target hardware address */
  uint32_t	arp_tpa;		/* target protocol address */
} __attribute__ ((packed));

} // namespace dperf