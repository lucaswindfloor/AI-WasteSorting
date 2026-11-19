#pragma once

#include "tracking_types.h"
#include <map>

namespace waste::m04 {

struct ModelPolicy {
    m03::ModelIdentity identity;
    m03::ScoreKind material_score_kind{m03::ScoreKind::Probability};
    float min_material_score{};
    // Map model-specific labels to a canonical material name.
    std::map<std::string, std::string> materials;
};

struct Limits {
    std::size_t max_targets{}, max_window_instances{}, max_window_samples{}, max_instance_points{};
    std::size_t max_pending_samples{}, max_readings_per_sample{}, max_recent_windows{};
};

struct TrackingConfig {
    m03::Generation generation;
    m02::Versions calibration;
    m02::EncoderConfig encoder;
    std::vector<ModelPolicy> models;
    Limits limits;
    Tick max_target_age{}, max_evidence_age{}, max_encoder_age{}, max_clock_error{};
    double max_association_distance_mm{}, min_shared_fraction{};
    std::size_t min_instance_points{}, min_material_samples{};
    bool allow_geometry_only{};
    double ownership_radius_mm{}, min_material_fraction{}, max_surface_gap_mm{};
    double min_surface_height_mm{};
    double max_prediction_distance_mm{}, max_prediction_error_mm{}, slip_mm_per_mm{};
    int belt_direction{1};
};

void validate(const TrackingConfig& config);
bool same_calibration(const m02::Versions& a, const m02::Versions& b);
Tick expires_at(Tick time, Tick age);
bool valid_reference(const TrackingConfig& config, const Reference& reference);
const ModelPolicy* model_policy(const TrackingConfig& config, const m03::ModelIdentity& identity);

}  // namespace waste::m04
