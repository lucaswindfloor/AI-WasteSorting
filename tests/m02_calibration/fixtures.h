#pragma once

#include "m02_calibration/calibration_service.h"

#include <cmath>
#include <stdexcept>

namespace waste::m02::test {

using namespace std::chrono_literals;

inline void check(bool passed, const char* message) {
    if (!passed) {
        throw std::runtime_error(message);
    }
}

inline bool near(double a, double b) {
    return std::abs(a - b) < 1e-8;
}

inline m01::Config acquisition_config(std::uint64_t generation = 1) {
    m01::Config c;
    c.run_id = "P0";
    c.generation = generation;
    c.sync = {"sync", 11, "mock", "mock", {}, true};
    c.rgb = {"rgb", 12, "mock", "mock", {"RGB8", "packed", 2, 1, {}, {}, {}}, true};
    c.hsi = {"hsi", 13, "mock", "mock", {"U16LE", "packed", 2, 1, {"500nm"}, {}, {}}, true};
    c.e05 = m01::SourceConfig{"depth", 14, "mock", "mock",
                             {"HEIGHT_F32LE", "packed", 2, 2, {}, "E05", "mm"}, true};
    c.encoder_epoch = 7;
    c.match_timeout = 5ms;
    c.segment_timeout = 3ms;
    c.source_stale_timeout = 50ms;
    c.max_pending = c.recent_history = 8;
    c.max_segment_items = 1;
    c.max_inflight_segments = 8;
    c.max_segment_bytes = c.max_packet_bytes = 32;
    c.max_packet_items = 4;
    c.buffer_capacity = 256;
    c.buffer_high_water = 200;
    c.contiguous_trigger_ids = true;
    return c;
}

inline Snapshot calibration(const m01::Config& c) {
    Snapshot s;
    s.run_id = c.run_id;
    s.generation = c.generation;
    s.versions = {"geo", "clock", "encoder", "white1"};
    const auto add = [&](m01::Source source, const m01::SourceConfig& d, std::string domain,
                         std::string basis) {
        s.devices.push_back({source, d.device_id, d.firmware, d.sdk, d.session, d.format});
        s.clocks.push_back({source, d.session, std::move(domain), std::move(basis), 0,
                            0, 100'000'000, 0ns, 1, 0ns});
    };
    add(m01::Source::Sync, c.sync, "sync", "trigger");
    add(m01::Source::Rgb, c.rgb, "rgb", "exposure_midpoint");
    add(m01::Source::Hsi, c.hsi, "hsi", "exposure_midpoint");
    add(m01::Source::E05, *c.e05, "depth", "exposure_midpoint");
    s.encoder = {c.encoder_epoch, 0.05, 0.01, 0.1, 0.01, 11ms, 10'000, 200, {}};
    s.rgb = {{{0, {{0, 406, 0}, {0, 410, 0}}},
              {50, {{0, 408, 50}, {0, 412, 50}}}}, 0.05};
    s.hsi = {{{0, {{0.2, 406, 0}, {0.2, 410, 0}}}}, 0.05};
    const Bounds bounds{{-200, 0, -5}, {200, 500, 100}};
    s.depth = DepthCalibration{{}, bounds, bounds, 0.2, {0, 405, 0}, 1, 1};
    RobotCalibration robot;
    robot.base_from_belt.translation = {100, 20, 5};
    robot.belt_bounds = bounds;
    robot.tool_id = "vacuum";
    robot.flange_from_tcp.translation.z = 40;
    robot.residual_mm = 0.2;
    s.robot = robot;
    s.spectral = SpectralCalibration{"preset-A", SpectralLevel::Raw, ReferenceLayout::PerBand,
                                     {"500nm"}, {110}, {2510}, 10, 4095, 15, 35, 1, 10, 20ms};
    s.max_exposure_separation = 2ms;
    s.max_reference_separation = 20ms;
    s.max_pair_dx_mm = 0.5;
    s.max_pair_dy_mm = 0.5;
    return s;
}

inline m01::TriggerEvent trigger(std::uint64_t id, m01::Tick time, std::int64_t count) {
    return {{11, 7, id}, count, 1, m01::SourceTime{"sync", time.count()}};
}

inline m01::Observation row(m01::Source source, std::uint64_t session, std::string domain,
                            m01::Tick time, std::vector<std::byte> bytes,
                            std::optional<m01::TriggerKey> key = {}) {
    m01::Observation result;
    result.header.id = {source, session, 1, 0};
    result.header.trigger = key;
    result.header.association_verified = key.has_value();
    result.header.time_basis = "exposure_midpoint";
    result.header.source_time = m01::SourceTime{std::move(domain), time.count()};
    result.header.bytes = {0, bytes.size()};
    result.received = time + 50ms;
    result.payload = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    return result;
}

inline Environment environment(const m01::Config& c) {
    return {c.run_id, c.generation, "preset-A", 1ms, 23, 5, true};
}

void test_time();
void test_space();
void test_spectrum();

}  // namespace waste::m02::test
