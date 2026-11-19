#pragma once

#include "calibration_types.h"

namespace waste::m02 {

CountEstimate locked_count(const EncoderConfig& config, const m01::TriggerEvent& trigger);
std::optional<CountEstimate> observation_count(const EncoderConfig& config,
                                               const m01::ObservationHeader& header,
                                               const std::optional<TimeEstimate>& time,
                                               std::span<const CountSample> samples);
std::optional<BeltShift> belt_shift(const EncoderConfig& config,
                                    const CountEstimate& from, const CountEstimate& to);

}  // namespace waste::m02
