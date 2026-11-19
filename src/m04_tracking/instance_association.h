#pragma once

#include "belt_prediction.h"

namespace waste::m04 {

struct Instance {
    Origin origin;
    Footprint footprint;
};

std::optional<Footprint> make_footprint(const TrackingConfig& config, std::vector<Sample> points);
std::vector<AssociationEvent> associate(const TrackingConfig& config,
                                        std::span<const Instance> observations,
                                        std::span<const Target* const> targets);

struct Ownership {
    std::optional<TargetId> target;
    std::vector<TargetId> plausible;
};

// Uses distances to observed support, without filling holes in a contour.
Ownership locate_owner(const TrackingConfig& config, const Sample& sample,
                       std::span<const Target* const> targets);

}  // namespace waste::m04
