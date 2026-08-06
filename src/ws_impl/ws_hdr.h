#pragma once

#include "common.h"
namespace axio {
struct ws_hdr {
    uint8_t workload_type_;
    size_t segment_num_;
};
} // namespace axio