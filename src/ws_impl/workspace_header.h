#pragma once

#include <type_traits>

#include "common.h"

namespace axio {

struct WorkspaceHeader {
  uint8_t workload_type_;
  size_t segment_num_;
};

static_assert(std::is_standard_layout_v<WorkspaceHeader>);

}  // namespace axio
