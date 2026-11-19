#pragma once

#include "evidence_index.h"

namespace waste::m04 {

Material fuse_material(const TrackingConfig& config, std::span<const SpectralPoint* const> owned,
                       std::vector<SampleKey> ambiguous);

}  // namespace waste::m04
