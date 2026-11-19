#pragma once

#include "../m03_inference/fixtures.h"
#include "m04_tracking/tracking_service.h"

namespace waste::m04::test {

using namespace std::chrono_literals;
using m02::test::check;
using m02::test::near;

inline TrackingConfig settings(std::uint64_t generation = 1) {
    const auto acquisition = m02::test::acquisition_config(generation);
    const auto snapshot = m02::test::calibration(acquisition);
    TrackingConfig c;
    c.generation = {acquisition.run_id, acquisition.generation};
    c.calibration = snapshot.versions;
    c.encoder = snapshot.encoder;
    c.models = {{m03::test::manifest(m03::Route::Joint).identity, m03::ScoreKind::Probability,
                 0.5f, {{"PET", "PET"}, {"PP", "PP"}}}};
    c.limits = {12, 12, 512, 128, 128, 8, 64};
    c.max_target_age = 300ms;
    c.max_evidence_age = 250ms;
    c.max_encoder_age = 150ms;
    c.max_clock_error = 1ms;
    c.max_association_distance_mm = 2;
    c.min_shared_fraction = 0.5;
    c.min_instance_points = 1;
    c.min_material_samples = 1;
    c.ownership_radius_mm = 2;
    c.min_material_fraction = 0.75;
    c.max_surface_gap_mm = 2;
    c.min_surface_height_mm = 2;
    c.max_prediction_distance_mm = 400;
    c.max_prediction_error_mm = 10;
    c.slip_mm_per_mm = 0.01;
    return c;
}

inline Reference ref(Tick time = 10ms, long double count = 33815) {
    return {time, 1ns, 7, count, 0.1};
}

inline EncoderState encoder(const TrackingConfig& c, Tick time, long double count) {
    return {c.generation, ref(time, count), c.calibration.encoder, 1, true, true};
}

inline m03::SampleLocation reading(m01::Source source, std::uint64_t frame, std::size_t column,
                                   double y, Tick time = 10ms, long double count = 33815,
                                   double x = 0) {
    const bool rgb = source == m01::Source::Rgb;
    return {{frame + 100, {11, 7, 2147}, {source, rgb ? 12U : 13U, frame, 0}, {0, 12}, column},
            {m01::Quality::Valid, {x, y, 0}, 0.1}, {time, 1ns},
            m02::CountEstimate{7, count, 0.1}, {}, 0};
}

inline m03::Evidence bottle(std::size_t instance, double y = 406, std::string label = "PET",
                            Tick time = 10ms) {
    const auto col = static_cast<std::size_t>(y);
    return {instance, std::move(label), m03::EvidenceKind::JointInstance,
            m03::ScoreKind::Probability, 0.9f,
            {reading(m01::Source::Rgb, 7872, col, y, time),
             reading(m01::Source::Hsi, 467, col, y, time)}};
}

inline m03::Result window(const TrackingConfig& c, std::uint64_t first,
                           std::vector<m03::Evidence> evidence) {
    m03::Result r{};
    r.window = {c.generation, c.models[0].identity.key, first, first + 63};
    r.model = c.models[0].identity;
    r.calibration = c.calibration;
    r.state = m03::State::Completed;
    r.deadline = 80ms;
    r.evidence = std::move(evidence);
    return r;
}

inline Sample sample(m01::Source source, std::size_t column, double y,
                     Tick time = 10ms, long double count = 33815, double x = 0) {
    return {{{{source, 12, 1234, 0}, column}, 100, {0, 12}}, ref(time, count), {x, y, 0}, 0.1};
}

void association_test();
void evidence_test();
void prediction_test();

}  // namespace waste::m04::test
