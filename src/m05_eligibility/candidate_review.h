#pragma once

#include "bin_router.h"
#include "tool_check.h"

namespace waste::m05 {

Check review(const Candidate& candidate, const m04::Target& target, const Recipe& recipe,
             const Snapshot& snapshot, Tick now);

}  // namespace waste::m05
