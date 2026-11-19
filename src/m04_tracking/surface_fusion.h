#pragma once

#include "belt_prediction.h"

namespace waste::m04 {

std::vector<SurfacePatch> fuse_surface(const TrackingConfig& config, const Reference& reference,
                                       std::span<const Sample* const> owned);

}  // namespace waste::m04
