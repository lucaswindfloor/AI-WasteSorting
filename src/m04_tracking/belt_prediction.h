#pragma once

#include "tracking_config.h"

namespace waste::m04 {

struct Displacement {
    double mm{}, error{};
};

std::optional<Displacement> displacement(const TrackingConfig& config,
                                         const Reference& from, const Reference& to);
std::optional<Sample> at_reference(const TrackingConfig& config, const Sample& point,
                                    const Reference& reference);
Tick reference_expiry(const Reference& reference, Tick max_age);
Reason encoder_issue(const TrackingConfig& config, const EncoderState& state, Tick now);
Prediction predict_position(const TrackingConfig& config, const Target& target,
                             const EncoderState* encoder, Tick now);

}  // namespace waste::m04
