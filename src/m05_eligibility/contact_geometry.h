#pragma once

#include "recipe_rules.h"

namespace waste::m05 {

struct SurfacePoint {
    const m04::Sample* sample{};
    Tick valid_until{};
};

struct SurfaceIndex {
    std::map<m04::SampleKey, SurfacePoint> points;
    Reason issue{Reason::None};
};

struct ContactGrid {
    const SurfaceGrid* source{};
    std::vector<SurfacePoint> points;
    Reason issue{Reason::None};
    double max_error_mm{};
};

SurfaceIndex index_surface(const m04::Target& target, std::size_t max_points);
ContactGrid prepare_grid(const m04::Target& target, const SurfaceGrid& grid,
                          const Recipe& recipe, const SurfaceIndex& index, Tick now);
Contact evaluate_contact(const ContactGrid& grid, const Region& region,
                          const CoverageProfile& profile, const ToolRule& tool, Tick now,
                          std::size_t& remaining_work);

}  // namespace waste::m05
