#include "fixtures.h"
#include "m01_acquisition/acquisition_service.h"

#include <cstdlib>
#include <iostream>

namespace waste::m02::test {
namespace {

std::shared_ptr<const m01::Segment> acquire(const m01::Config& c) {
    m01::AcquisitionService acquisition;
    acquisition.start(c, 0ms);
    const auto sync = trigger(2147, 10ms, 1000);
    acquisition.ingest(m01::SyncInput{c.generation, sync, 12ms}, 12ms);
    const auto rgb = row(m01::Source::Rgb, 12, "rgb", 11ms,
                         std::vector<std::byte>(6), sync.key);
    const auto hsi = row(m01::Source::Hsi, 13, "hsi", 12ms,
                         {std::byte{0x1e}, std::byte{0x05}, std::byte{0x1e}, std::byte{0x05}}, sync.key);
    acquisition.ingest(m01::PacketView{c.generation, m01::Source::Rgb, 12, c.rgb.format, *rgb.payload,
                                      std::span(&rgb.header, 1), 13ms}, 13ms);
    auto out = acquisition.ingest(m01::PacketView{c.generation, m01::Source::Hsi, 13, c.hsi.format,
                                                *hsi.payload, std::span(&hsi.header, 1), 14ms}, 14ms);
    check(out.segments.size() == 1, "M01 did not emit the paired input");
    return out.segments.front();
}

void service_integration() {
    const auto c = acquisition_config();
    auto original = calibration(c);
    CalibrationService service;
    service.install(original);
    const auto segment = acquire(c);
    const auto state = environment(c);
    std::array samples{trigger(2148, 20ms, 1020)};
    const SyncWindow window{c.run_id, c.generation, samples};
    const ProcessContext context{&window, &state, nullptr, {}};
    const auto result = service.process(segment, context);
    check(result.triggers.size() == 1 && result.triggers[0].common_coverage.size() == 2,
          "M01 data did not reach common geometric coverage");
    const auto& group = result.triggers[0];
    check(group.rgb && group.hsi && group.rgb->shift && near(group.rgb->shift->mm, -0.1) &&
          group.reference_count.value == 1000 && group.hsi->spectrum &&
          near(group.hsi->spectrum->samples[0], 0.5) && group.hsi->source.received == 14ms,
          "M01 provenance, shift or spectrum lost at the service interface");

    const auto no_counter = service.process(segment);
    check(no_counter.triggers[0].common_coverage.empty() &&
          no_counter.triggers[0].rgb->issue == "unverified_exposure_count" &&
          no_counter.triggers[0].rgb->measured.size() == 2, "missing count evidence hid valid geometry");

    auto missing = std::make_shared<m01::Segment>(*segment);
    auto& missing_row = std::get<m01::TriggerRecord>(missing->items[0]);
    missing_row.hsi.reset();
    missing_row.hsi_quality = Quality::Missing;
    const auto partial = service.process(missing, context);
    check(partial.triggers[0].common_coverage.empty() && !partial.triggers[0].hsi &&
          partial.triggers[0].source.hsi_quality == Quality::Missing, "source gap was converted to valid coverage");

    CoordinateReference reference{c.run_id, c.generation, "geo", 10ms, group.reference_count};
    PoseRequest pose{reference, "vacuum", {}, PoseTarget::Tcp};
    pose.belt_from_tcp.translation = {0, 406, 10};
    const auto robot = service.transform_pose(pose);
    check(robot.value && robot.reference.count.value == 1000 && robot.reference.time == 10ms &&
          near(robot.value->translation.x, 100), "transform query applied a second belt shift");
    auto request = PointRequest{reference, Frame::Belt, Frame::RobotBase, {0, 406, 10}};
    check(service.transform_point(request).value.has_value(), "point transform not available");
    request.reference.geometry_version = "other";
    check(service.transform_point(request).issue == TransformIssue::GeometryVersionMismatch,
          "point transform used the wrong version");
    pose.tool_id = "other";
    check(service.transform_pose(pose).issue == TransformIssue::ToolMismatch,
          "pose transform provided no tool error");

    auto independent = std::make_shared<m01::Segment>(*segment);
    const auto depth = row(m01::Source::E05, 14, "depth", 15ms, std::vector<std::byte>(8));
    independent->items.push_back(depth);
    const auto reference_event = trigger(2147, 10ms, 1000);
    const ProcessContext depth_context{&window, &state, &reference_event, {}};
    const auto surface = service.process(independent, depth_context);
    check(surface.independent.size() == 1 && surface.independent[0].shift &&
          near(surface.independent[0].aligned[0].position.x, -0.5) &&
          surface.independent[0].reference_time->value == 10ms,
          "independent depth retained the wrong time, count or position");

    const auto next = acquisition_config(2);
    auto replacement = calibration(next);
    replacement.versions.geometry = "geo2";
    auto invalid = replacement;
    invalid.rgb.layers.clear();
    try {
        service.install(invalid);
        throw std::logic_error("bad snapshot was installed");
    } catch (const std::invalid_argument&) {
    }
    check(service.process(acquire(next)).issue == "no_matching_calibration", "failed install leaked into new generation");
    service.install(replacement);
    check(service.process(acquire(next)).snapshot->versions.geometry == "geo2" &&
          service.process(segment).snapshot->versions.geometry == "geo", "queued segment bound to new geometry");
    service.retire(c.run_id, c.generation);
    check(service.process(segment).issue == "no_matching_calibration" &&
          result.snapshot->versions.geometry == "geo", "retirement corrupted the existing evidence");
    try {
        service.install(original);
        throw std::logic_error("retired identity was silently reused");
    } catch (const std::invalid_argument&) {
    }

    const auto rollback = acquisition_config(3);
    original.generation = 3;
    service.install(original);
    check(service.process(acquire(rollback)).snapshot->versions.geometry == "geo",
          "rollback under a new generation lost its version");
}

}  // namespace
}  // namespace waste::m02::test

int main() {
    using namespace waste::m02::test;
    try {
        test_time();
        test_space();
        test_spectrum();
        service_integration();
        std::cout << "M02: time, geometry, spectrum, and M01 integration passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
