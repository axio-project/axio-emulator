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

#include <cstddef>
#include <cstdint>

#include "dispatcher_impl/ethhdr.h"

namespace axio {

inline constexpr uint16_t kEtherTypeArp = 0x0806;
inline constexpr uint16_t kEtherTypeIpv4 = 0x0800;
inline constexpr uint16_t kArpHardwareEthernet = 1;
inline constexpr uint16_t kArpOperationRequest = 1;
inline constexpr uint16_t kArpOperationReply = 2;

struct __attribute__((packed)) ArpHeader {
  uint16_t hardware_type_;
  uint16_t protocol_type_;
  uint8_t hardware_address_length_;
  uint8_t protocol_address_length_;
  uint16_t operation_;
  uint8_t sender_hardware_address_[kEthernetAddressLength];
  uint32_t sender_protocol_address_;
  uint8_t target_hardware_address_[kEthernetAddressLength];
  uint32_t target_protocol_address_;
};

static_assert(sizeof(ArpHeader) == 28);
static_assert(offsetof(ArpHeader, operation_) == 6);
static_assert(offsetof(ArpHeader, sender_hardware_address_) == 8);
static_assert(offsetof(ArpHeader, sender_protocol_address_) == 14);
static_assert(offsetof(ArpHeader, target_hardware_address_) == 18);
static_assert(offsetof(ArpHeader, target_protocol_address_) == 24);

}  // namespace axio
