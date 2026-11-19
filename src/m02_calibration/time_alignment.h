#pragma once

#include "calibration_types.h"

namespace waste::m02 {

std::optional<TimeEstimate> sync_time(const Snapshot& snapshot, const m01::TriggerEvent& trigger);
std::optional<TimeEstimate> observation_time(const Snapshot& snapshot,
                                             const m01::ObservationHeader& header,
                                             const m01::TriggerRecord* trigger = nullptr);
bool within_time(const TimeEstimate& a, const TimeEstimate& b, m01::Tick limit);

}  // namespace waste::m02
