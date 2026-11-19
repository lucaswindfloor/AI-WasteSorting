#pragma once

#include "recipe_rules.h"

namespace waste::m05 {

ToolCheck check_tool(const ToolIdentity& expected, const Snapshot& snapshot, Tick now, Tick max_age);

}  // namespace waste::m05
