#include "tracking_config.h"

#include <cmath>
#include <set>
#include <stdexcept>

namespace waste::m04 {
namespace {

bool positive(double value) {
    return std::isfinite(value) && value > 0;
}

bool fraction(double value) {
    return positive(value) && value <= 1;
}

}  // namespace

void validate(const TrackingConfig& c) {
    const auto& limits = c.limits;
    if (c.generation.run_id.empty() || c.calibration.geometry.empty() || c.calibration.clock.empty() ||
        c.calibration.encoder.empty() || c.models.empty() || !limits.max_targets ||
        !limits.max_window_instances || !limits.max_window_samples || !limits.max_instance_points ||
        limits.max_instance_points > limits.max_window_samples || !limits.max_pending_samples ||
        !limits.max_readings_per_sample || !limits.max_recent_windows ||
        !c.min_instance_points || c.min_instance_points > limits.max_instance_points ||
        !c.min_material_samples || c.min_material_samples > limits.max_pending_samples) {
        throw std::invalid_argument("tracking identity, model set or capacity is invalid");
    }
    if (c.max_target_age <= Tick::zero() || c.max_evidence_age <= Tick::zero() ||
        c.max_encoder_age <= Tick::zero() || c.max_clock_error < Tick::zero() ||
        !positive(std::abs(c.encoder.signed_mm_per_count)) ||
        !positive(c.max_association_distance_mm) || !positive(c.ownership_radius_mm) ||
        !positive(c.max_surface_gap_mm) || !std::isfinite(c.min_surface_height_mm) || !positive(c.max_prediction_distance_mm) ||
        !positive(c.max_prediction_error_mm) || !std::isfinite(c.slip_mm_per_mm) ||
        c.slip_mm_per_mm < 0 || !fraction(c.min_shared_fraction) ||
        !fraction(c.min_material_fraction) ||
        (c.belt_direction != 1 && c.belt_direction != -1)) {
        throw std::invalid_argument("tracking timing, geometry, motion or fusion policy is invalid");
    }
    std::set<m03::ModelKey> keys;
    for (const auto& policy : c.models) {
        if (policy.identity.key.package.empty() || policy.identity.key.version.empty() ||
            policy.identity.artifact_digest.empty() || policy.identity.backend.empty() ||
            policy.identity.precision.empty() || policy.identity.processing_version.empty() ||
            !keys.insert(policy.identity.key).second || !std::isfinite(policy.min_material_score) ||
            (policy.material_score_kind != m03::ScoreKind::Score &&
             policy.material_score_kind != m03::ScoreKind::Probability) ||
            (policy.material_score_kind == m03::ScoreKind::Probability &&
             (policy.min_material_score < 0 || policy.min_material_score > 1))) {
            throw std::invalid_argument("tracking model or material threshold is invalid");
        }
        for (const auto& [label, canonical] : policy.materials) {
            if (label.empty() || canonical.empty()) {
                throw std::invalid_argument("empty label in material mapping");
            }
        }
    }
}

bool same_calibration(const m02::Versions& a, const m02::Versions& b) {
    return a.geometry == b.geometry && a.clock == b.clock && a.encoder == b.encoder &&
           a.spectral_reference == b.spectral_reference;
}

Tick expires_at(Tick time, Tick age) {
    return Tick::max() - time < age ? Tick::max() : time + age;
}

bool valid_reference(const TrackingConfig& c, const Reference& r) {
    return r.encoder_epoch == c.encoder.epoch && r.time >= Tick::zero() &&
           r.time_error >= Tick::zero() && r.time_error <= c.max_clock_error &&
           std::isfinite(r.count) && std::isfinite(r.count_error) && r.count_error >= 0;
}

const ModelPolicy* model_policy(const TrackingConfig& c, const m03::ModelIdentity& identity) {
    const auto found = std::ranges::find(c.models, identity, &ModelPolicy::identity);
    return found == c.models.end() ? nullptr : &*found;
}

}  // namespace waste::m04
