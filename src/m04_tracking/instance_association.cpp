#include "instance_association.h"

#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <utility>

namespace waste::m04 {
namespace {

std::size_t shared_points(const Footprint& a, const Footprint& b) {
    auto left = a.points.begin();
    auto right = b.points.begin();
    std::size_t shared = 0;
    while (left != a.points.end() && right != b.points.end()) {
        if (left->source.key < right->source.key) {
            ++left;
        } else if (right->source.key < left->source.key) {
            ++right;
        } else {
            ++shared;
            ++left;
            ++right;
        }
    }
    return shared;
}

bool overlaps_axis(double v, double min, double max, double margin) {
    return v >= min - margin && v <= max + margin;
}

}  // namespace

std::optional<Footprint> make_footprint(const TrackingConfig& c, std::vector<Sample> points) {
    if (points.empty()) {
        return {};
    }
    const auto latest = std::ranges::max_element(points, {}, [](const Sample& sample) {
        return sample.reference.time;
    })->reference;
    std::ranges::stable_sort(points, {}, [](const Sample& p) { return p.source.key; });
    Footprint result;
    result.reference = latest;
    for (const auto& point : points) {
        if (!result.points.empty() && result.points.back().source.key == point.source.key) {
            continue;
        }
        if (auto aligned = at_reference(c, point, latest)) {
            result.points.push_back(std::move(*aligned));
        }
    }
    if (result.points.size() < c.min_instance_points) {
        return {};
    }
    result.min = result.max = result.points.front().position;
    for (const auto& point : result.points) {
        result.min.x = std::min(result.min.x, point.position.x);
        result.min.y = std::min(result.min.y, point.position.y);
        result.min.z = std::min(result.min.z, point.position.z);
        result.max.x = std::max(result.max.x, point.position.x);
        result.max.y = std::max(result.max.y, point.position.y);
        result.max.z = std::max(result.max.z, point.position.z);
        result.error_mm = std::max(result.error_mm, point.error_mm);
    }
    result.center = {std::midpoint(result.min.x, result.max.x), std::midpoint(result.min.y, result.max.y),
                     std::midpoint(result.min.z, result.max.z)};
    return result;
}

std::vector<AssociationEvent> associate(const TrackingConfig& c,
                                        std::span<const Instance> observations,
                                        std::span<const Target* const> targets) {
    std::vector<AssociationEvent> result;
    std::vector<std::optional<TargetId>> confirmed;
    std::map<TargetId, std::size_t> incoming;
    for (const auto& observation : observations) {
        const auto& a = observation.footprint;
        AssociationEvent event{observation.origin, {}, {}, Reason::None};
        std::optional<TargetId> unique;
        for (const auto* target : targets) {
            const auto& b = target->footprint;
            const auto shared = shared_points(a, b);
            const auto movement = displacement(c, b.reference, a.reference);
            bool plausible = shared != 0;
            bool reliable = false;
            if (movement) {
                const auto delta = std::hypot(a.center.x - (b.center.x + movement->mm),
                                                a.center.y - b.center.y);
                const auto error = a.error_mm + b.error_mm + movement->error;
                plausible |= delta <= c.max_association_distance_mm + error;
                const auto minimum = std::min(a.points.size(), b.points.size());
                const auto enough = shared >= c.min_shared_fraction * minimum;
                reliable = delta + error <= c.max_association_distance_mm &&
                           (enough || (c.allow_geometry_only && !shared));
            }
            if (plausible) {
                event.candidates.push_back(target->id);
                ++incoming[target->id];
                if (reliable) {
                    unique = target->id;
                }
            }
        }
        confirmed.push_back(unique);
        result.push_back(std::move(event));
    }

    for (std::size_t i = 0; i < result.size(); ++i) {
        auto& event = result[i];
        if (event.candidates.size() == 1 && confirmed[i] && incoming[*confirmed[i]] == 1) {
            event.matched = confirmed[i];
        } else if (!event.candidates.empty()) {
            event.reason = Reason::AssociationAmbiguous;
        }
        for (std::size_t j = i + 1; j < result.size(); ++j) {
            if (shared_points(observations[i].footprint, observations[j].footprint)) {
                result[i].matched.reset();
                result[i].reason = Reason::AssociationAmbiguous;
                result[j].matched.reset();
                result[j].reason = Reason::AssociationAmbiguous;
                confirmed[j].reset();
            }
        }
    }
    return result;
}

Ownership locate_owner(const TrackingConfig& c, const Sample& sample,
                       std::span<const Target* const> targets) {
    Ownership result;
    for (const auto* target : targets) {
        const auto& f = target->footprint;
        const auto point = at_reference(c, sample, f.reference);
        if (!point) {
            continue;
        }
        const auto margin = c.ownership_radius_mm + point->error_mm + f.error_mm;
        if (!overlaps_axis(point->position.x, f.min.x, f.max.x, margin) ||
            !overlaps_axis(point->position.y, f.min.y, f.max.y, margin)) {
            continue;
        }
        bool plausible = false, verified = false;
        for (const auto& support : f.points) {
            const auto distance = std::hypot(point->position.x - support.position.x,
                                              point->position.y - support.position.y);
            const auto error = point->error_mm + support.error_mm;
            plausible |= distance <= c.ownership_radius_mm + error;
            verified |= distance + error <= c.ownership_radius_mm;
        }
        if (plausible) {
            result.plausible.push_back(target->id);
            if (verified && target->state == TargetState::Tracked) {
                result.target = target->id;
            }
        }
    }
    if (result.plausible.size() != 1) {
        result.target.reset();
    }
    return result;
}

}  // namespace waste::m04
