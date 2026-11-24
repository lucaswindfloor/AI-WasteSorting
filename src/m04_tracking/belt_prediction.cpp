#include "belt_prediction.h"

#include <cmath>

namespace waste::m04 {

std::optional<Displacement> displacement(const TrackingConfig& c,
                                         const Reference& from, const Reference& to) {
    if (!valid_reference(c, from) || !valid_reference(c, to)) {
        return {};
    }
    if (from == to) {
        return Displacement{};
    }
    const auto value = (to.count - from.count) * c.encoder.signed_mm_per_count;
    const auto error = (from.count_error + to.count_error) * std::abs(c.encoder.signed_mm_per_count) +
                       std::abs(value) * c.slip_mm_per_mm;
    if (!std::isfinite(static_cast<double>(value)) || !std::isfinite(static_cast<double>(error)) ||
        std::abs(value) + error > c.max_prediction_distance_mm) {
        return {};
    }
    return Displacement{static_cast<double>(value), static_cast<double>(error)};
}

std::optional<Sample> at_reference(const TrackingConfig& c, const Sample& point,
                                    const Reference& reference) {
    const auto shift = displacement(c, point.reference, reference);
    if (!shift) {
        return {};
    }
    auto result = point;
    result.source_reference = point.source_reference.value_or(point.reference);
    result.reference = reference;
    result.position.x += shift->mm;
    result.error_mm += shift->error;
    if (!std::isfinite(result.position.x) || !std::isfinite(result.error_mm)) {
        return {};
    }
    return result;
}

Tick reference_expiry(const Reference& reference, Tick max_age) {
    const auto start = std::max(Tick::zero(), reference.time - reference.time_error);
    return expires_at(start, max_age);
}

Reason encoder_issue(const TrackingConfig& c, const EncoderState& state, Tick now) {
    if (state.reference.encoder_epoch != c.encoder.epoch) {
        return Reason::EncoderReset;
    }
    if (!state.valid || state.generation != c.generation ||
        state.encoder_version != c.calibration.encoder || !valid_reference(c, state.reference) ||
        state.reference.time > now) {
        return Reason::EncoderInvalid;
    }
    if (!state.following_verified || (state.direction != 0 && state.direction != c.belt_direction)) {
        return Reason::MotionUnverified;
    }
    if (now >= reference_expiry(state.reference, c.max_encoder_age)) {
        return Reason::EncoderStale;
    }
    return Reason::None;
}

Prediction predict_position(const TrackingConfig& c, const Target& target,
                             const EncoderState* encoder, Tick now) {
    Prediction result{target.id, target.version, target.generation, Reason::None,
                      target.footprint.reference, {}, target.footprint.center, 0,
                      target.footprint.error_mm, target.valid_until};
    if (target.state != TargetState::Tracked) {
        result.reason = target.reason;
    } else if (now >= target.valid_until) {
        result.reason = Reason::TargetExpired;
    } else if (!encoder) {
        result.reason = Reason::EncoderUnavailable;
    } else {
        result.current = encoder->reference;
        result.reason = encoder_issue(c, *encoder, now);
        if (result.reason == Reason::None) {
            result.fresh_until = reference_expiry(encoder->reference, c.max_encoder_age);
            const auto movement = displacement(c, result.origin, result.current);
            if (result.current.time < result.origin.time) {
                result.reason = Reason::EncoderUnavailable;
            } else if (!movement || result.error_mm + movement->error > c.max_prediction_error_mm) {
                result.reason = Reason::OutsideRange;
            } else if (movement->mm * c.belt_direction < -movement->error) {
                result.reason = Reason::MotionUnverified;
            } else {
                result.applied_mm = movement->mm;
                result.error_mm += movement->error;
                result.position.x += movement->mm;
                if (!std::isfinite(result.position.x)) {
                    result.reason = Reason::OutsideRange;
                }
            }
        }
    }
    return result;
}

}  // namespace waste::m04
