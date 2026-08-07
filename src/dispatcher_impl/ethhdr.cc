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

#include "ethhdr.h"

#include <cstdio>

namespace axio {
void format_ethernet_address(const EthernetAddress* address, char* output) {
  const uint8_t* bytes = address->bytes_;
  std::snprintf(output, kEthernetAddressStringLength + 1,
                "%x:%x:%x:%x:%x:%x", static_cast<unsigned int>(bytes[0]),
                static_cast<unsigned int>(bytes[1]),
                static_cast<unsigned int>(bytes[2]),
                static_cast<unsigned int>(bytes[3]),
                static_cast<unsigned int>(bytes[4]),
                static_cast<unsigned int>(bytes[5]));
}

int parse_ethernet_address(EthernetAddress* address, const char* input) {
  unsigned int bytes[kEthernetAddressLength];

  if (std::strlen(input) != kEthernetAddressStringLength) {
    return -1;
  }
  const int parsed_count =
      std::sscanf(input, "%x:%x:%x:%x:%x:%x", &bytes[0], &bytes[1],
                  &bytes[2], &bytes[3], &bytes[4], &bytes[5]);
  if (parsed_count != static_cast<int>(kEthernetAddressLength)) {
    return -1;
  }

  for (size_t i = 0; i < kEthernetAddressLength; ++i) {
    if (bytes[i] > 0xff) {
      return -1;
    }
    address->bytes_[i] = static_cast<uint8_t>(bytes[i]);
  }

  return 0;
}

}  // namespace axio
