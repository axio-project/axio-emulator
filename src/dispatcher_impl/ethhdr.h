/*
 * Copyright (c) 2021 Baidu.com, Inc. All Rights Reserved.
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
 */

#pragma once

#include <arpa/inet.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace axio {

inline constexpr size_t kEthernetAddressLength = 6;
inline constexpr size_t kEthernetAddressStringLength = 17;

struct __attribute__((packed)) EthernetAddress {
  uint8_t bytes_[kEthernetAddressLength];
};

struct __attribute__((packed)) EthernetHeader {
  EthernetAddress destination_;
  EthernetAddress source_;
  uint16_t ether_type_;
};

static_assert(sizeof(EthernetAddress) == kEthernetAddressLength);
static_assert(sizeof(EthernetHeader) == 14);
static_assert(offsetof(EthernetHeader, destination_) == 0);
static_assert(offsetof(EthernetHeader, source_) == 6);
static_assert(offsetof(EthernetHeader, ether_type_) == 12);

inline bool ethernet_address_is_zero(const EthernetAddress* address) {
  return address->bytes_[0] == 0 && address->bytes_[1] == 0 &&
         address->bytes_[2] == 0 && address->bytes_[3] == 0 &&
         address->bytes_[4] == 0 && address->bytes_[5] == 0;
}

inline void copy_ethernet_address(EthernetAddress* destination,
                                  const EthernetAddress* source) {
  std::memcpy(destination, source, sizeof(EthernetAddress));
}

inline void swap_ethernet_addresses(EthernetHeader* header) {
  EthernetAddress temporary_address;
  copy_ethernet_address(&temporary_address, &header->destination_);
  copy_ethernet_address(&header->destination_, &header->source_);
  copy_ethernet_address(&header->source_, &temporary_address);
}

inline void initialize_ethernet_header(EthernetHeader* header,
                                       uint16_t ether_type,
                                       const EthernetAddress* destination,
                                       const EthernetAddress* source) {
  header->ether_type_ = htons(ether_type);
  copy_ethernet_address(&header->destination_, destination);
  copy_ethernet_address(&header->source_, source);
}

void format_ethernet_address(const EthernetAddress* address, char* output);
int parse_ethernet_address(EthernetAddress* address, const char* input);

}  // namespace axio
