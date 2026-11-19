#include "contact_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace waste::m05 {
namespace {

Contact refused(State state, Reason reason) {
    Contact result;
    result.check = {state, reason, {}};
    result.reasons.push_back(reason);
    return result;
}

bool valid_state(CellState state) {
    return state == CellState::Observed || state == CellState::Missing ||
           state == CellState::Neighbor || state == CellState::OutsideTarget;
}

Reason issue_for(CellState state) {
    switch (state) {
    case CellState::OutsideTarget: return Reason::ContactOutside;
    case CellState::Neighbor: return Reason::NeighborOverlap;
    case CellState::Missing: return Reason::SurfaceMissing;
    case CellState::Observed: return Reason::None;
    }
    return Reason::InvalidSurface;
}

double interval_distance(double v, double lo, double hi) {
    return std::max({0.0, lo - v, v - hi});
}

std::size_t cell_index(double coordinate, double origin, double pitch, std::size_t extent) {
    const auto index = std::floor((coordinate - origin) / pitch);
    return static_cast<std::size_t>(std::clamp(index, 0.0, static_cast<double>(extent - 1)));
}

}  // namespace

SurfaceIndex index_surface(const m04::Target& target, std::size_t max_points) {
    SurfaceIndex result;
    std::size_t seen = 0;
    for (const auto& patch : target.surfaces) {
        if (patch.points.size() > max_points - seen) {
            result.issue = Reason::Capacity;
            break;
        }
        seen += patch.points.size();
        for (const auto& sample : patch.points) {
            if (sample.source.key.observation.source != m01::Source::E05 ||
                sample.reference != target.footprint.reference || !finite(sample.position.x) ||
                !finite(sample.position.y) || !finite(sample.position.z) ||
                !finite(sample.error_mm) || sample.error_mm < 0 || patch.valid_until <= Tick::zero()) {
                result.issue = Reason::InvalidSurface;
                return result;
            }
            const auto [entry, inserted] = result.points.emplace(sample.source.key,
                                                                  SurfacePoint{&sample, patch.valid_until});
            if (!inserted) {
                if (*entry->second.sample != sample) {
                    result.issue = Reason::InvalidSurface;
                    return result;
                }
                entry->second.valid_until = std::min(entry->second.valid_until, patch.valid_until);
            }
        }
    }
    return result;
}

ContactGrid prepare_grid(const m04::Target& target, const SurfaceGrid& grid,
                          const Recipe& recipe, const SurfaceIndex& index, Tick now) {
    if (index.issue != Reason::None) {
        return {&grid, {}, index.issue, 0};
    }
    const auto& profile = recipe.coverage;
    const auto right = grid.x0 + grid.rows * profile.pitch_x_mm;
    const auto top = grid.y0 + grid.columns * profile.pitch_y_mm;
    if (grid.target != binding(target) || grid.proof_id.empty() ||
        grid.profile_version != profile.version || !finite(grid.x0) || !finite(grid.y0) ||
        !finite(right) || !finite(top) || grid.x0 + profile.pitch_x_mm <= grid.x0 ||
        grid.y0 + profile.pitch_y_mm <= grid.y0 || right - profile.pitch_x_mm >= right ||
        top - profile.pitch_y_mm >= top ||
        !grid.rows || !grid.columns || grid.rows > recipe.limits.max_cells / grid.columns ||
        grid.cells.size() != grid.rows * grid.columns) {
        return {&grid, {}, Reason::InvalidSurface, 0};
    }
    if (now >= grid.valid_until) {
        return {&grid, {}, Reason::SurfaceExpired, 0};
    }

    ContactGrid result{&grid, {}, Reason::None, 0};
    result.points.resize(grid.cells.size());
    std::set<m04::SampleKey> seen;
    for (std::size_t i = 0; i < grid.cells.size(); ++i) {
        const auto& cell = grid.cells[i];
        if (!valid_state(cell.state) || (cell.state == CellState::Observed) != cell.source.has_value() ||
            (cell.tilt_upper_deg && (cell.state != CellState::Observed || !finite(*cell.tilt_upper_deg) ||
                                     *cell.tilt_upper_deg < 0 || *cell.tilt_upper_deg > 90))) {
            return {&grid, {}, Reason::InvalidSurface, 0};
        }
        if (!cell.source) {
            continue;
        }
        const auto found = index.points.find(*cell.source);
        if (found == index.points.end() || !seen.insert(*cell.source).second) {
            return {&grid, {}, Reason::InvalidSurface, 0};
        }
        const auto& point = *found->second.sample;
        const auto x = grid.x0 + (i / grid.columns + 0.5) * profile.pitch_x_mm;
        const auto y = grid.y0 + (i % grid.columns + 0.5) * profile.pitch_y_mm;
        if (std::hypot(point.position.x - x, point.position.y - y) > profile.center_tolerance_mm) {
            return {&grid, {}, Reason::InvalidSurface, 0};
        }
        result.points[i] = found->second;
        result.max_error_mm = std::max(result.max_error_mm, point.error_mm);
    }
    return result;
}

Contact evaluate_contact(const ContactGrid& prepared, const Region& region,
                          const CoverageProfile& profile, const ToolRule& tool, Tick now,
                          std::size_t& remaining_work) {
    if (prepared.issue != Reason::None) {
        const auto soft = prepared.issue == Reason::SurfaceExpired || prepared.issue == Reason::Capacity;
        return refused(soft ? State::Pending : State::Rejected, prepared.issue);
    }
    if (!prepared.source || !finite(region.x) || !finite(region.y) || region.id.empty()) {
        return refused(State::Rejected, Reason::InvalidSurface);
    }
    const auto& grid = *prepared.source;
    const auto rx = profile.pitch_x_mm;
    const auto ry = profile.pitch_y_mm;
    const auto contact_radius = tool.radius_mm + tool.pose_error_mm + prepared.max_error_mm;
    const auto radius = contact_radius + tool.edge_margin_mm;
    auto result = Contact{};
    result.required_radius_mm = radius;
    const auto bound_x = grid.x0 + grid.rows * rx;
    const auto bound_y = grid.y0 + grid.columns * ry;
    if (!finite(radius) || region.x - radius < grid.x0 || region.x + radius > bound_x ||
        region.y - radius < grid.y0 || region.y + radius > bound_y) {
        result.check = {State::Rejected, Reason::ContactOutside, {}};
        result.reasons.push_back(Reason::ContactOutside);
        return result;
    }

    const auto first_row = cell_index(std::nextafter(region.x - radius, -std::numeric_limits<double>::infinity()),
                                      grid.x0, rx, grid.rows);
    const auto last_row = cell_index(region.x + radius, grid.x0, rx, grid.rows);
    const auto first_column = cell_index(std::nextafter(region.y - radius, -std::numeric_limits<double>::infinity()),
                                         grid.y0, ry, grid.columns);
    const auto last_column = cell_index(region.y + radius, grid.y0, ry, grid.columns);
    const auto work = (last_row - first_row + 1) * (last_column - first_column + 1);
    if (work > remaining_work) {
        return refused(State::Pending, Reason::Capacity);
    }
    remaining_work -= work;
    auto until = grid.valid_until;
    auto low = std::numeric_limits<double>::infinity();
    auto high = -low;
    bool hard_failure = false;
    const auto add = [&](Reason reason, bool hard = false) {
        if (std::ranges::find(result.reasons, reason) == result.reasons.end()) {
            result.reasons.push_back(reason);
        }
        if (hard && !hard_failure) {
            result.check.reason = reason;
        }
        hard_failure |= hard;
    };

    for (auto row = first_row; row <= last_row; ++row) {
        const auto left = grid.x0 + row * rx;
        for (auto col = first_column; col <= last_column; ++col) {
            const auto bottom = grid.y0 + col * ry;
            const auto distance = std::hypot(interval_distance(region.x, left, left + rx),
                                              interval_distance(region.y, bottom, bottom + ry));
            if (distance > radius) {
                continue;
            }
            const auto index = row * grid.columns + col;
            const auto& cell = grid.cells[index];
            if (cell.state != CellState::Observed) {
                add(issue_for(cell.state), cell.state != CellState::Missing);
                continue;
            }
            const auto& evidence = prepared.points[index];
            if (now >= evidence.valid_until) {
                add(Reason::SurfaceExpired);
                continue;
            }
            until = std::min(until, evidence.valid_until);
            result.sources.push_back(evidence.sample->source);
            if (distance <= contact_radius) {
                const auto& point = *evidence.sample;
                const auto error = point.error_mm + profile.within_cell_height_mm;
                low = std::min(low, point.position.z - error);
                high = std::max(high, point.position.z + error);
                if (cell.tilt_upper_deg) {
                    result.tilt_upper_deg = std::max(result.tilt_upper_deg.value_or(0), *cell.tilt_upper_deg);
                } else if (tool.max_tilt_deg) {
                    add(Reason::NormalMissing);
                }
            }
        }
    }
    const auto r = cell_index(region.x, grid.x0, rx, grid.rows);
    const auto c = cell_index(region.y, grid.y0, ry, grid.columns);
    const auto& center = prepared.points[r * grid.columns + c];
    if (center.sample && now < center.valid_until) {
        result.center = m04::Point{region.x, region.y, center.sample->position.z};
    } else {
        add(Reason::CenterMissing);
    }
    if (low <= high) {
        result.height_delta_mm = high - low;
        if (!finite(result.height_delta_mm) || result.height_delta_mm > tool.max_height_delta_mm) {
            add(Reason::HeightVariation, true);
        }
    }
    if (tool.max_tilt_deg && result.tilt_upper_deg && *result.tilt_upper_deg > *tool.max_tilt_deg) {
        add(Reason::TiltExceeded, true);
    }

    const auto seen = [&](Reason reason) {
        return std::ranges::find(result.reasons, reason) != result.reasons.end();
    };
    if (seen(Reason::SurfaceMissing)) {
        result.missing.push_back(MissingComponent::Coverage);
    }
    if (seen(Reason::CenterMissing)) {
        result.missing.push_back(MissingComponent::CenterHeight);
    }
    if (seen(Reason::NormalMissing)) {
        result.missing.push_back(MissingComponent::Normal);
    }
    if (result.reasons.empty()) {
        result.check = {State::Ready, Reason::None, until};
    } else if (hard_failure) {
        result.check.state = State::Rejected;
    } else {
        result.check = {State::Pending, result.reasons.front(), until};
    }
    return result;
}

}  // namespace waste::m05
