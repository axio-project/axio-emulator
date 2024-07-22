#pragma once

#include "common.h"
namespace dperf {
struct ws_hdr {
    uint8_t workload_type_;
    size_t segment_num_;
};
} // namespace dperf