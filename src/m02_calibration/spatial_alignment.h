#pragma once

#include "calibration_types.h"

namespace waste::m02 {

std::vector<ProjectedPoint> project_scan(const ScanCalibration& scan, double height_mm);
std::vector<ProjectedPoint> project_depth(const Snapshot& snapshot, const m01::DataFormat& format,
                                          const m01::Observation& source);
std::vector<ProjectedPoint> shift_points(std::span<const ProjectedPoint> measured, const BeltShift& shift);
std::vector<Coverage> overlap(const Snapshot& snapshot, const AlignedObservation& rgb,
                              const AlignedObservation& hsi);

}  // namespace waste::m02
