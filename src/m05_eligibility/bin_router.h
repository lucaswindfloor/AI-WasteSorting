#pragma once

#include "recipe_rules.h"

namespace waste::m05 {

struct BinRoute {
    Check check;
    std::vector<BinCheck> checks;
    std::optional<std::size_t> selected;
};

BinRoute route_bin(const MaterialRule& rule, const Snapshot& snapshot, Tick now, Tick max_age);

}  // namespace waste::m05
