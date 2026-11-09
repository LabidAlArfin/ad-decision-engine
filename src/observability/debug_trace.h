#pragma once

#include <string>
#include <vector>

#include "common/types.h"

namespace ad {

using TraceId = u64;

struct StageDrop {
  std::string stage;
  std::string reason;
};

struct DebugTrace {
  TraceId id = 0;
  std::vector<StageDrop> drops;
};

}
