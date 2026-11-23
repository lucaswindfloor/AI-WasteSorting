#include "fixtures.h"
#include "fake_backend.h"
#include "m01_acquisition/acquisition_service.h"

#include <cstdlib>
#include <iostream>

namespace waste::m03::test {
namespace {

constexpr std::array artifact{std::byte{1}};

BackendCompletion hit() {
    return {{{0, 0.9f, {1, 0, 2, 4}, {}}}, {}};
}

std::vector<m02::AlignedTrigger> rows(std::uint64_t start) {
    return {aligned_fixture(start), aligned_fixture(start + 1),
            aligned_fixture(start + 2), aligned_fixture(start + 3)};
}

void actual_upstream() {
    const auto c = m02::test::acquisition_config();
    auto s = m02::test::calibration(c);
    s.spectral->environment_max_age = 100ms;
    m01::AcquisitionService acquisition;
    acquisition.start(c, 0ms);
    m02::CalibrationService calibration;
    calibration.install(s);
    std::array<m01::TriggerEvent, 5> sync;
    for (std::size_t i = 0; i < sync.size(); ++i) {
        sync[i] = m02::test::trigger(2147 + i, 10ms * (i + 1), 1000 + 20 * i);
    }
    const m02::SyncWindow window{c.run_id, c.generation, sync};
    const auto environment = m02::test::environment(c);
    const m02::ProcessContext context{&window, &environment, nullptr, {}};
    FakeBackend backend;
    backend.controls->output = hit();
    InferenceService service(2, 2, {2, 1, 1024 * 1024});
    auto m = manifest(Route::Joint);
    m.window.max_wait = 40ms;
    service.install(m, artifact, backend);
    service.activate(c, s, {2147, {m.identity.key}});
    Report ready;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto time = 10ms * (i + 1);
        acquisition.ingest(m01::SyncInput{c.generation, sync[i], time}, time);
        auto rgb = m02::test::row(m01::Source::Rgb, 12, "rgb", time + 1ms,
                                  std::vector<std::byte>(6), sync[i].key);
        auto hsi = m02::test::row(m01::Source::Hsi, 13, "hsi", time + 2ms,
                                  {std::byte{0x1e}, std::byte{5}, std::byte{0x1e}, std::byte{5}}, sync[i].key);
        rgb.header.id.frame = hsi.header.id.frame = i;
        acquisition.ingest(m01::PacketView{c.generation, m01::Source::Rgb, 12, c.rgb.format,
                                          *rgb.payload, std::span(&rgb.header, 1), time + 3ms}, time + 3ms);
        const auto packet = acquisition.ingest(
            m01::PacketView{c.generation, m01::Source::Hsi, 13, c.hsi.format,
                            *hsi.payload, std::span(&hsi.header, 1), time + 4ms}, time + 4ms);
        check(packet.segments.size() == 1, "M01 did not publish the paired row");
        auto paired = std::make_shared<m02::Result>(calibration.process(packet.segments.front(), context));
        check(paired->triggers.front().common_coverage.size() == 2, "M02 did not align shared columns");
        ready = service.ingest(std::move(paired), time + 4ms, 80ms);
        if (i < 3) {
            check(ready.windows.empty(), "incomplete real-source window emitted");
        }
    }
    check(ready.windows.size() == 1 && ready.windows[0].state == State::Completed && ready.windows[0].reason.empty() &&
          ready.windows[0].evidence[0].samples.size() == 8 &&
          ready.windows[0].evidence[0].samples[1].source.observation.source == m01::Source::Hsi,
          "M01/M02/M03 integration lost joint window evidence");
    const auto& quality = ready.windows.front().input_quality;
    check(quality && quality->missing_triggers.empty() && quality->rgb && quality->hsi &&
          quality->rgb->valid == 24 && quality->rgb->total == 24 &&
          quality->hsi->valid == 8 && quality->hsi->total == 8, "window sample quality was lost");
}

void version_and_deadline() {
    const auto c = m02::test::acquisition_config();
    const auto s = m02::test::calibration(c);
    const auto next = m02::test::acquisition_config(2);
    const auto s2 = m02::test::calibration(next);
    const auto m = manifest(Route::Joint);
    const auto m2 = manifest(Route::Joint, "2");
    FakeBackend slow, fast;
    slow.controls->delay = 30ms;
    slow.controls->output = fast.controls->output = hit();
    InferenceService service(4, 2, {1, 1, 1024 * 1024});
    service.install(m, artifact, slow);
    service.install(m2, artifact, fast);
    service.activate(c, s, {2147, {m.identity.key}});
    check(service.ingest(segment(c, s, {aligned_fixture(2149), aligned_fixture(2147),
                                          aligned_fixture(2150)}), 1ms, 9ms).windows.empty(),
          "missing unconfirmed slot collapsed");
    const auto no_hsi = service.ingest(segment(c, s, {aligned_fixture(2148, true)}), 2ms, 9ms);
    check(no_hsi.windows.empty() && slow.controls->submitted == 1 &&
          !slow.controls->latest.expired(), "allowed missing HSI was not submitted with mask");
    const auto pending = slow.controls->latest.lock();
    check(pending && pending->source_map[9].rgb->source.trigger.id == 2149 &&
          !pending->source_map[5].hsi, "missing 2148 moved the joint input rows");

    service.activate(next, s2, {2151, {m2.identity.key}});
    check(service.deployment_status().active.at({c.run_id, 2})[0].key.version == "2",
          "actual active version unavailable to M08");
    const auto old_tail = service.retire({c.run_id, 1}, 3ms, true);
    check(old_tail.windows.size() == 1 && old_tail.windows[0].state == State::Retired &&
          !service.unload(m.identity.key), "old job or crossing window was not managed at boundary");
    check(service.ingest(segment(next, s2, rows(2151)), 4ms, 60ms).windows.empty() &&
          service.health().waiting == 1, "new window not queued behind active job");
    const auto full = service.ingest(segment(next, s2, {aligned_fixture(2155), aligned_fixture(2156)}),
                                     5ms, 61ms);
    check(full.windows.size() == 1 && full.windows[0].state == State::QueueFull &&
          full.windows[0].window.first == 2153, "queue full lost affected window");
    const auto expired = service.advance(9ms);
    check(expired.windows.size() == 1 && expired.windows[0].state == State::Expired &&
          expired.windows[0].model.key.version == "1" && service.health().running == 1 &&
          service.health().retained_bytes >= pending->retained_bytes,
          "expired job was dropped before the backend released it");
    const auto finished = service.advance(32ms);
    const auto success = std::ranges::find(finished.windows, State::Completed, &Result::state);
    check(success != finished.windows.end() && success->model.key.version == "2" &&
          success->window.first == 2151 &&
          std::ranges::count(finished.windows, State::Completed, &Result::state) == 1 &&
          service.health().running == 0 && service.health().retained_bytes == 0 &&
          service.unload(m.identity.key), "late old output leaked or new version failed to run");
}

void status_and_clock() {
    const auto c = m02::test::acquisition_config();
    const auto s = m02::test::calibration(c);
    auto m = manifest();
    m.window.step = m.window.rows;
    FakeBackend backend;
    InferenceService service(2, 1, {2, 1, 1024 * 1024});
    service.install(m, artifact, backend);
    service.activate(c, s, {2147, {m.identity.key}});
    auto report = service.ingest(segment(c, s, rows(2147)), 1ms, 100ms);
    check(report.windows.size() == 1 && report.windows[0].state == State::NoDetection &&
          report.windows[0].reason == "no_valid_candidates",
          "valid empty output was not distinguished");
    backend.controls->output = {{{4, 0.8f, {1, 0, 2, 1}, {}}}, {}};
    report = service.ingest(segment(c, s, rows(2151)), 2ms, 100ms);
    check(report.windows.size() == 1 && report.windows[0].state == State::Failed,
          "illegal backend output looked like no detection");
    report = service.ingest(segment(c, s, {aligned_fixture(2155)}), 3ms, 100ms);
    check(report.windows.empty(), "unconfirmed source gap treated as settled");
    report = service.seal_through({c.run_id, 1}, 2158, 4ms);
    check(report.windows.size() == 1 && report.windows[0].state == State::InputInvalid,
          "a confirmed missing row window produced no invalid-input report");

    FakeBackend late;
    late.controls->output = hit();
    auto tick = 0ms;
    auto reads = 0;
    InferenceQueue queue({1, 1, 10000}, [&]() {
        return ++reads >= 5 ? 10ms : tick;
    });
    ModelRegistry registry(1);
    registry.install(m, artifact, late);
    auto input = std::make_shared<PreparedInput>();
    input->width = m.raster.width();
    input->height = m.raster.height();
    input->source_map.resize(input->width * input->height);
    Result provenance{};
    provenance.deadline = 5ms;
    Report timing;
    queue.enqueue({std::move(provenance), registry.find(m.identity.key), input}, 0ms, timing);
    queue.advance(0ms, timing);
    check(timing.windows.size() == 1 && timing.windows[0].state == State::Expired,
          "wall clock passage during result handling was not used");
}

}  // namespace
}  // namespace waste::m03::test

int main() {
    using namespace waste::m03::test;
    try {
        manifest_test();
        builder_test();
        preprocessor_test();
        projection_test();
        actual_upstream();
        version_and_deadline();
        status_and_clock();
        std::cout << "M03: 7 scenarios passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
