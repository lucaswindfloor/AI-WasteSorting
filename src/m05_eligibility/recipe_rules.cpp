#include "recipe_rules.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace waste::m05 {
namespace {

bool nonnegative(double value) { return finite(value) && value >= 0; }
bool positive(double value) { return nonnegative(value) && value > 0; }

bool proper_tool(const ToolIdentity& tool) {
    return !tool.station.empty() && !tool.tool.empty() && !tool.configuration.empty() && !tool.tcp.empty();
}

void verify_route(const Recipe& recipe, const MaterialRule& route) {
    if (route.route_label.empty() || route.tools.empty() || route.bins.empty() ||
        route.tools.size() > recipe.limits.max_options || route.bins.size() > recipe.limits.max_options) {
        throw std::invalid_argument("material route has missing or excessive options");
    }
    std::set<ToolIdentity> tools;
    for (const auto& tool : route.tools) {
        if (!recipe.tools.contains(tool) || !tools.insert(tool).second) {
            throw std::invalid_argument("material route has an unknown or duplicate tool");
        }
    }
    std::set<std::string> slots;
    std::set<std::string> bins;
    for (const auto& bin : route.bins) {
        if (bin.slot.empty() || bin.physical_bin.empty() || !slots.insert(bin.slot).second ||
            !bins.insert(bin.physical_bin).second) {
            throw std::invalid_argument("material route has an invalid or duplicate bin");
        }
    }
    for (auto condition : route.fallback_after) {
        if (condition != BinCondition::Full && condition != BinCondition::Absent &&
            condition != BinCondition::Unavailable && condition != BinCondition::Changing) {
            throw std::invalid_argument("invalid bin fallback condition");
        }
    }
}

}  // namespace

bool finite(double value) { return std::isfinite(value); }

void validate(const Recipe& r) {
    const auto& l = r.limits;
    constexpr std::size_t max_cells = 1024 * 1024;
    if (r.generation.run_id.empty() || r.version.empty() || r.bin_mapping_version.empty() ||
        r.coverage.version.empty() || r.valid_until <= Tick::zero() || r.max_fact_age <= Tick::zero() ||
        r.tools.empty() || (r.materials.empty() && !r.unknown) ||
        !l.max_points || l.max_points > max_cells || !l.max_cells || l.max_cells > max_cells ||
        !l.max_grids || l.max_grids > 64 || !l.max_regions || l.max_regions > 256 ||
        !l.max_facts || l.max_facts > 1024 || !l.max_options || l.max_options > 32 ||
        r.tools.size() > l.max_facts || !l.max_contact_checks || l.max_contact_checks > 4 * max_cells) {
        throw std::invalid_argument("invalid recipe identity, time or capacity");
    }
    const auto& p = r.coverage;
    if (!positive(p.pitch_x_mm) || !positive(p.pitch_y_mm) ||
        !nonnegative(p.center_tolerance_mm) || !nonnegative(p.within_cell_height_mm) ||
        p.center_tolerance_mm > std::min(p.pitch_x_mm, p.pitch_y_mm) / 2) {
        throw std::invalid_argument("invalid surface coverage profile");
    }
    for (const auto& [id, tool] : r.tools) {
        if (id != tool.identity || !proper_tool(id) || !positive(tool.radius_mm) ||
            !nonnegative(tool.edge_margin_mm) || !nonnegative(tool.pose_error_mm) ||
            !nonnegative(tool.max_height_delta_mm) ||
            (tool.max_tilt_deg && (!nonnegative(*tool.max_tilt_deg) || *tool.max_tilt_deg > 90))) {
            throw std::invalid_argument("invalid tool geometry or version");
        }
    }
    for (const auto& [label, route] : r.materials) {
        if (label.empty() || route.route_label != label) {
            throw std::invalid_argument("material route label mismatch");
        }
        verify_route(r, route);
    }
    if (r.unknown) {
        if (r.unknown->statuses.empty() || r.materials.contains(r.unknown->route.route_label) ||
            std::ranges::any_of(r.unknown->statuses, [](auto status) {
                return status != m04::MaterialStatus::Insufficient &&
                       status != m04::MaterialStatus::Conflict &&
                       status != m04::MaterialStatus::AmbiguousOwnership;
            })) {
            throw std::invalid_argument("invalid explicit route for unknown material");
        }
        verify_route(r, r.unknown->route);
    }
    for (auto item : r.remeasurement_components) {
        if (item != MissingComponent::Coverage && item != MissingComponent::CenterHeight &&
            item != MissingComponent::Normal) {
            throw std::invalid_argument("invalid remeasurement component");
        }
    }
}

Check check_target(const m04::Target& t, const Recipe& r, Tick now) {
    if (t.generation != r.generation) {
        return {State::Rejected, Reason::WrongGeneration, {}};
    }
    if (now < Tick::zero() || !t.id || !t.version || t.state == m04::TargetState::Invalid ||
        t.footprint.reference.time < Tick::zero() || t.footprint.reference.time > now ||
        t.footprint.reference.time_error < Tick::zero() || !std::isfinite(t.footprint.reference.count) ||
        !finite(t.footprint.reference.count_error) || t.footprint.reference.count_error < 0) {
        return {State::Rejected, Reason::InvalidTarget, {}};
    }
    if (now >= r.valid_until) {
        return {State::Pending, Reason::RecipeChanged, {}};
    }
    if (now >= t.valid_until) {
        return {State::Pending, Reason::TargetExpired, {}};
    }
    if (t.state != m04::TargetState::Tracked) {
        return {State::Pending, Reason::AmbiguousTarget, {}};
    }
    return {State::Ready, Reason::None, std::min(t.valid_until, r.valid_until)};
}

RouteChoice choose_material(const m04::Target& t, const Recipe& r, Tick now) {
    const auto& m = t.material;
    if (m.status == m04::MaterialStatus::Known) {
        if (!m.valid_until || now >= *m.valid_until || m.supporting.empty()) {
            return {nullptr, {State::Pending, Reason::MaterialExpired, {}}};
        }
        const auto route = r.materials.find(m.label);
        if (route == r.materials.end()) {
            return {nullptr, {State::Rejected, Reason::MaterialExcluded, {}}};
        }
        return {&route->second, {State::Ready, Reason::None, std::min(*m.valid_until, r.valid_until)}};
    }
    if (r.unknown && std::ranges::find(r.unknown->statuses, m.status) != r.unknown->statuses.end()) {
        return {&r.unknown->route, {State::Ready, Reason::None, r.valid_until}};
    }
    return {nullptr, {State::Pending, m.status == m04::MaterialStatus::Conflict
                                      ? Reason::MaterialConflict : Reason::MaterialUnknown, {}}};
}

Tick usable_until(const FactTime& time, Tick now, Tick max_age) {
    if (time.source.empty() || time.observed < Tick::zero() || time.observed > now ||
        time.valid_until <= time.observed || now >= time.valid_until ||
        max_age <= Tick::zero() || now - time.observed >= max_age) {
        return Tick::zero();
    }
    const auto age_limit = Tick::max() - time.observed < max_age
                           ? Tick::max() : time.observed + max_age;
    return std::min(time.valid_until, age_limit);
}

bool snapshot_identity_valid(const Snapshot& snapshot, const Recipe& r) {
    if (!snapshot.revision || snapshot.generation != r.generation ||
        snapshot.bin_mapping_version != r.bin_mapping_version ||
        snapshot.tools.size() > r.limits.max_facts || snapshot.bins.size() > r.limits.max_facts) {
        return false;
    }
    std::set<std::string> stations, slots, bins;
    std::set<std::string> mounted_tools;
    for (const auto& tool : snapshot.tools) {
        if (tool.identity.station.empty() || !stations.insert(tool.identity.station).second ||
            (tool.condition == ToolCondition::Ready &&
             (!proper_tool(tool.identity) || !mounted_tools.insert(tool.identity.tool).second))) {
            return false;
        }
    }
    for (const auto& bin : snapshot.bins) {
        if (bin.identity.slot.empty() || !slots.insert(bin.identity.slot).second ||
            (bin.condition == BinCondition::Ready &&
             (bin.identity.physical_bin.empty() || !bins.insert(bin.identity.physical_bin).second))) {
            return false;
        }
    }
    return true;
}

}  // namespace waste::m05
