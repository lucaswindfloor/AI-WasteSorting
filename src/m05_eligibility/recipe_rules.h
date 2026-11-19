#pragma once

#include "eligibility_types.h"

#include <map>

namespace waste::m05 {

struct ToolRule {
    ToolIdentity identity;
    // P0 uses a vertical circular suction footprint.
    double radius_mm{}, edge_margin_mm{}, pose_error_mm{}, max_height_delta_mm{};
    std::optional<double> max_tilt_deg;
};

struct MaterialRule {
    std::string route_label;
    std::vector<ToolIdentity> tools;
    // Ordered from primary to approved fallback destinations.
    std::vector<BinIdentity> bins;
    std::vector<BinCondition> fallback_after;
    bool allow_remeasurement{};
};

struct UnknownRule {
    std::vector<m04::MaterialStatus> statuses;
    MaterialRule route;
};

struct CoverageProfile {
    // Identifier of the validated method that certifies whole measured cells.
    std::string version;
    double pitch_x_mm{}, pitch_y_mm{}, center_tolerance_mm{}, within_cell_height_mm{};
};

struct Limits {
    std::size_t max_points{}, max_cells{}, max_grids{}, max_regions{}, max_facts{}, max_options{};
    std::size_t max_contact_checks{};
};

struct Recipe {
    m03::Generation generation;
    std::string version, bin_mapping_version;
    CoverageProfile coverage;
    std::map<ToolIdentity, ToolRule> tools;
    std::map<std::string, MaterialRule> materials;
    std::optional<UnknownRule> unknown;
    std::vector<MissingComponent> remeasurement_components;
    Limits limits;
    Tick valid_until{}, max_fact_age{};
};

struct RouteChoice {
    const MaterialRule* route{};
    Check check;
};

void validate(const Recipe& recipe);
Check check_target(const m04::Target& target, const Recipe& recipe, Tick now);
RouteChoice choose_material(const m04::Target& target, const Recipe& recipe, Tick now);
bool finite(double value);
bool snapshot_identity_valid(const Snapshot& snapshot, const Recipe& recipe);
Tick usable_until(const FactTime& time, Tick now, Tick max_age);

}  // namespace waste::m05
