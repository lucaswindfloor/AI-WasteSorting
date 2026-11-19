#include "fixtures.h"

#include <cstdlib>
#include <iostream>

namespace waste::m05::test {
namespace {

void t042_precheck() {
    const auto t = target();
    const auto snapshot = facts();
    auto good = grid(t);
    auto incomplete = grid(t, "E05-proof-B");
    incomplete.cells[12] = {CellState::Missing};
    const std::array grids{good, incomplete};
    const std::array areas{Region{"A", 0, 406, 0}, Region{"B", 0, 406, 1}};
    const EligibilityService service(recipe());
    const auto result = service.evaluate(t, snapshot, {grids, areas}, 15ms);
    check(result.state == State::Ready && result.candidates.size() == 1 &&
          result.candidates.front().region_id == "A" && result.candidates.front().bin.identity == primary() &&
          result.candidates.front().target.reference.count == 33815 &&
          result.regions.size() == 2 && result.regions[1].contact.check.state == State::Pending &&
          result.snapshot_revision == 18 && result.material_sources == t.material.supporting,
          "T042 failed expected A/B checks or lost evidence");
    auto updated = t;
    ++updated.version;
    check(service.recheck(result.candidates[0], updated, snapshot, 17ms).reason == Reason::TargetVersionChanged,
          "out-of-date target candidate was accepted");
    auto r = recipe();
    r.version = "recipe-2";
    check(EligibilityService(r).recheck(result.candidates[0], t, snapshot, 17ms).reason == Reason::RecipeChanged,
          "old recipe candidate was accepted");
    check(service.recheck(result.candidates[0], t, snapshot, 60ms).reason == Reason::CandidateExpired,
          "candidate survived earliest upstream expiry");
}

void actual_m04_handoff() {
    auto tracking_config = m04::test::settings();
    tracking_config.ownership_radius_mm = 10;
    m04::TrackingService tracking;
    tracking.activate(tracking_config, 0ms);
    const auto tracked = tracking.ingest_window(
        m04::test::window(tracking_config, 2112, {m04::test::bottle(0)}), 12ms);
    check(tracked.targets.size() == 1 && tracked.targets[0].material.label == "PET",
          "M04 failed to build the expected target");
    auto input = m02::Result{};
    auto segment = std::make_shared<m01::Segment>();
    const auto acquisition = m02::test::acquisition_config();
    segment->run_id = acquisition.run_id;
    segment->generation = acquisition.generation;
    segment->sequence = 55;
    segment->config = std::make_shared<m01::Config>(acquisition);
    input.source = segment;
    input.snapshot = std::make_shared<m02::Snapshot>(m02::test::calibration(acquisition));
    for (std::uint32_t x = 0; x < 5; ++x) {
        m02::AlignedObservation observation;
        observation.source.header.id = {m01::Source::E05, 14, 22, x};
        observation.source.header.bytes = {0, 20};
        observation.source_quality = m01::Quality::Valid;
        observation.time = m02::TimeEstimate{12ms, 1ns};
        observation.count = m02::CountEstimate{7, 33815, 0.1};
        for (std::uint32_t y = 0; y < 5; ++y) {
            observation.measured.push_back({m01::Quality::Valid, {-2.0 + x, 404.0 + y, 48.6}, 0.05});
        }
        input.independent.push_back(std::move(observation));
    }
    const auto from_m04 = tracking.ingest_surfaces(input, 13ms);
    check(from_m04.targets.size() == 1 && from_m04.targets[0].surfaces.size() == 5,
          "M04 did not create all measured surface rows");
    const auto& current = from_m04.targets[0];
    const EligibilityService service(recipe());
    const auto result = evaluate(service, current, facts(), grid(current));
    check(result.candidates.size() == 1 && result.candidates[0].target.id == tracked.targets[0].id &&
          result.candidates[0].target.version == current.version &&
          result.candidates[0].contact.sources[0].segment == 55,
          "M05 result lost the actual M04 evidence or current target version");
}

}  // namespace
}  // namespace waste::m05::test

int main() {
    using namespace waste::m05::test;
    try {
        geometry_test();
        rules_test();
        bin_test();
        remeasurement_test();
        t042_precheck();
        actual_m04_handoff();
        std::cout << "M05: 6 scenarios passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
