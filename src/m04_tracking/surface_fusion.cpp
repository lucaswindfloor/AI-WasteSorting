#include "surface_fusion.h"

#include <cmath>
#include <limits>
#include <utility>

namespace waste::m04 {

std::vector<SurfacePatch> fuse_surface(const TrackingConfig& c, const Reference& reference,
                                       std::span<const Sample* const> owned) {
    std::vector<SurfacePatch> patches;
    // The evidence index enumerates source rows and columns in key order.
    for (const auto* measurement : owned) {
        const auto point = at_reference(c, *measurement, reference);
        if (!point) {
            continue;
        }
        const auto valid_until = reference_expiry(measurement->reference, c.max_evidence_age);
        if (!patches.empty()) {
            auto& patch = patches.back();
            const auto& last = patch.points.back();
            const auto& a = last.source.key;
            const auto& b = point->source.key;
            const auto distance = std::hypot(point->position.x - last.position.x,
                                              point->position.y - last.position.y,
                                              point->position.z - last.position.z);
            if (a.observation == b.observation && a.column != std::numeric_limits<std::size_t>::max() &&
                a.column + 1 == b.column &&
                distance + last.error_mm + point->error_mm <= c.max_surface_gap_mm) {
                patch.points.push_back(*point);
                patch.valid_until = std::min(patch.valid_until, valid_until);
                patch.min.x = std::min(patch.min.x, point->position.x);
                patch.min.y = std::min(patch.min.y, point->position.y);
                patch.min.z = std::min(patch.min.z, point->position.z);
                patch.max.x = std::max(patch.max.x, point->position.x);
                patch.max.y = std::max(patch.max.y, point->position.y);
                patch.max.z = std::max(patch.max.z, point->position.z);
                continue;
            }
        }
        patches.push_back({{*point}, point->position, point->position, valid_until});
    }
    return patches;
}

}  // namespace waste::m04
