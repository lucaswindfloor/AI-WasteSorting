#include "fixtures.h"
#include "../m03_inference/fake_backend.h"

#include <cstdlib>
#include <iostream>

namespace waste::m04::test {
namespace {

m02::Result surface(const TrackingConfig& c) {
    const auto acquisition = m02::test::acquisition_config(c.generation.config_epoch);
    m02::Result result;
    auto segment = std::make_shared<m01::Segment>();
    segment->run_id = c.generation.run_id;
    segment->generation = c.generation.config_epoch;
    segment->sequence = 50;
    segment->config = std::make_shared<m01::Config>(acquisition);
    result.source = segment;
    result.snapshot = std::make_shared<m02::Snapshot>(m02::test::calibration(acquisition));
    m02::AlignedObservation depth;
    depth.source.header.id = {m01::Source::E05, 14, 12, 0};
    depth.source.header.bytes = {0, 16};
    depth.source_quality = m01::Quality::Valid;
    depth.time = m02::TimeEstimate{12ms, 1ns};
    depth.count = m02::CountEstimate{7, 33815, 0.1};
    depth.measured = {{m01::Quality::Valid, {0, 406, 48.6}, 0.1},
                      {m01::Quality::Missing, {}, 0},
                      {m01::Quality::Valid, {0, 406, 0}, 0.1},
                      {m01::Quality::Valid, {0, 409.3, 21}, 0.1}};
    result.independent.push_back(std::move(depth));
    return result;
}

void t042_sequence() {
    const auto c = settings();
    TrackingService service;
    service.activate(c, 0ms);
    const auto depth = surface(c);
    const auto early = service.ingest_surfaces(depth, 13ms);
    check(early.targets.empty() && early.evidence.size() == 4 &&
          early.evidence[1].reason == Reason::SurfaceMissing &&
          early.evidence[2].reason == Reason::SurfaceUnassigned,
          "zero height treated as missing or E05 assigned without a target");
    m03::Evidence boundary{2, "PP", m03::EvidenceKind::Material, m03::ScoreKind::Probability,
                            0.99f, {reading(m01::Source::Hsi, 500, 999, 407.5)}};
    const auto first = service.ingest_window(window(c, 2112, {bottle(0), bottle(1, 409.3, "PP"),
                                                              std::move(boundary)}), 14ms);
    check(first.targets.size() == 2 && first.associations.size() == 2, "initial targets not created");
    const auto id = first.targets[0].id;
    const auto version = first.targets[0].version;
    const auto& pet = first.targets[0];
    const auto& pp = first.targets[1];
    check(pet.material.label == "PET" && pet.material.supporting.size() == 1 &&
          pp.material.label == "PP" && pp.material.supporting.size() == 1 &&
          pet.material.disputed.size() == 1 && pp.material.disputed.size() == 1,
          "boundary reading contaminated a neighboring label");
    check(pet.surfaces.size() == 1 && pet.surfaces[0].points.size() == 1 &&
          near(pet.surfaces[0].points[0].position.z, 48.6) &&
          pet.surfaces[0].valid_until == 262ms - 1ns &&
          pet.material.valid_until == 260ms - 1ns &&
          pp.surfaces.size() == 1 && near(pp.surfaces[0].points[0].position.z, 21),
          "E05 coverage or neighbor ownership was invented");

    auto w18 = window(c, 2144, {bottle(0), bottle(1, 409.3, "PP")});
    w18.evidence[0].samples[1].source.segment_sequence = 51;
    const auto overlap = service.ingest_window(w18, 15ms);
    check(overlap.associations[0].matched == id && overlap.targets.empty() &&
          service.target(id)->material.supporting.size() == 1,
          "overlap created a new target, version or duplicate spectral vote");
    const auto replay = service.ingest_window(w18, 16ms);
    check(replay.targets.empty() && replay.evidence.front().reason == Reason::Duplicate,
          "duplicate window was applied again");

    service.update_encoder(encoder(c, 20ms, 33815), 20ms);
    check(service.update_encoder(encoder(c, 110ms, 37815), 110ms).targets.empty(),
          "normal belt motion created a target version");
    const auto predicted = service.predict(id, version, 110ms);
    check(predicted.reason == Reason::None && near(predicted.applied_mm, 200) &&
          predicted.version == version, "target prediction is invalid");
    auto reset = encoder(c, 111ms, 0);
    reset.reference.encoder_epoch = 8;
    const auto invalid = service.update_encoder(reset, 111ms);
    check(invalid.targets.size() == 2 && invalid.targets[0].state == TargetState::Invalid &&
          invalid.targets[0].reason == Reason::EncoderReset &&
          !service.target(id) && service.predict(id, version, 111ms).reason == Reason::TargetMissing,
          "encoder reset left old targets usable");
    const auto replay_after_reset = service.update_encoder(encoder(c, 112ms, 37816), 112ms);
    check(replay_after_reset.evidence.front().reason == Reason::EncoderReset,
          "old epoch incorrectly rescued invalid targets");
}

void late_and_retired() {
    const auto c = settings();
    TrackingService service;
    service.activate(c, 0ms);
    const auto first = service.ingest_window(window(c, 2112, {bottle(0)}), 12ms);
    const auto id = first.targets[0].id;
    auto newest = bottle(0);
    newest.samples.push_back(reading(m01::Source::Rgb, 8000, 400, 406.2, 20ms));
    const auto update = service.ingest_window(window(c, 2120, {newest}), 21ms);
    check(update.targets.size() == 1 && update.targets[0].version == 2 &&
          update.targets[0].footprint.reference.time == 20ms,
          "newer measured basis did not create a target version");
    auto old = service.ingest_window(window(c, 2121, {bottle(0), bottle(1)}), 22ms);
    check(old.associations[0].reason == Reason::AssociationAmbiguous && old.targets.empty() &&
          service.target(id)->state == TargetState::Tracked &&
          service.target(id)->footprint.reference.time == 20ms,
          "older conflicting window changed a newer association");
    check(service.predict(id, 1, 22ms).reason == Reason::VersionMismatch,
          "old target version accepted");
    const auto invalid = service.invalidate_motion(id, 2, 25ms);
    check(invalid.targets.front().reason == Reason::MotionUnverified && !service.target(id),
          "per-item invalidation failed");
    old = service.ingest_window(window(c, 2122, {newest}), 26ms);
    check(old.targets.empty() && old.associations[0].reason == Reason::AssociationAmbiguous,
          "late data revived a retired contour");
    const auto retired_generation = settings(2);
    service.activate(retired_generation, 27ms);
    auto previous = service.ingest_window(window(c, 2123, {newest}), 28ms);
    check(previous.evidence.front().reason == Reason::WrongGeneration,
          "input from retired configuration accepted");
}

void recover_from_merge() {
    const auto c = settings();
    TrackingService service;
    service.activate(c, 0ms);
    const auto first = service.ingest_window(window(c, 2112, {bottle(0), bottle(1, 416, "PP")}), 12ms);
    const auto left = first.targets[0].id;
    const auto right = first.targets[1].id;
    auto merged = bottle(0);
    merged.samples.push_back(reading(m01::Source::Rgb, 7872, 416, 416));
    merged.samples.push_back(reading(m01::Source::Rgb, 7900, 407, 406.5, 20ms));
    const auto join = service.ingest_window(window(c, 2120, {merged}), 21ms);
    check(join.targets.size() == 2 && join.targets[0].state == TargetState::Ambiguous &&
          join.targets[1].state == TargetState::Ambiguous &&
          service.predict(left, join.targets[0].version, 21ms).reason == Reason::AssociationAmbiguous,
          "merge left at least one target eligible");
    auto a = bottle(0);
    auto b = bottle(1, 416, "PP");
    a.samples.push_back(reading(m01::Source::Rgb, 7901, 407, 406.5, 25ms));
    b.samples.push_back(reading(m01::Source::Rgb, 7901, 417, 416.5, 25ms));
    const auto split = service.ingest_window(window(c, 2121, {a, b}), 26ms);
    check(split.targets.size() == 2 && split.targets[0].id == left && split.targets[1].id == right &&
          split.targets[0].state == TargetState::Tracked && split.targets[1].state == TargetState::Tracked,
          "new separation evidence failed to restore the original identities");
}

void m03_handoff() {
    const auto c = settings();
    const auto acquisition = m02::test::acquisition_config();
    const auto calibrated = m02::test::calibration(acquisition);
    const auto manifest = m03::test::manifest(m03::Route::Joint);
    m03::test::FakeBackend backend;
    backend.controls->output = {{{0, 0.9f, {1, 0, 2, 4}, {}}}, {}};
    m03::InferenceService inference(1, 1, {1, 1, 1024 * 1024});
    const std::array artifact{std::byte{1}};
    inference.install(manifest, artifact, backend);
    inference.activate(acquisition, calibrated, {2147, {manifest.identity.key}});
    auto input = m03::test::segment(acquisition, calibrated, {m03::test::aligned_fixture(2147),
                                   m03::test::aligned_fixture(2148), m03::test::aligned_fixture(2149),
                                   m03::test::aligned_fixture(2150)});
    const auto output = inference.ingest(std::move(input), 15ms, 70ms);
    check(output.windows.size() == 1 && output.windows[0].state == m03::State::Completed,
          "M03 fixture did not publish model evidence");
    TrackingService tracking;
    tracking.activate(c, 0ms);
    const auto tracked = tracking.ingest_window(output.windows[0], 16ms);
    check(tracked.targets.size() == 1 && tracked.targets[0].material.label == "PET" &&
          tracked.targets[0].material.supporting.size() == 4 &&
          tracked.targets[0].footprint.points.size() == 4, "M03 samples not fused by M04");
}

}  // namespace
}  // namespace waste::m04::test

int main() {
    using namespace waste::m04::test;
    try {
        association_test();
        evidence_test();
        prediction_test();
        t042_sequence();
        late_and_retired();
        recover_from_merge();
        m03_handoff();
        std::cout << "M04: 7 scenarios passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
