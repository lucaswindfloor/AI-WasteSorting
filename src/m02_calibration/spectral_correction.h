#pragma once

#include "calibration_types.h"

namespace waste::m02 {

SpectralRow correct_spectrum(const Snapshot& snapshot, const m01::DataFormat& format,
                             const m01::Observation& observation,
                             const std::optional<TimeEstimate>& exposure,
                             const Environment* environment);

}  // namespace waste::m02
