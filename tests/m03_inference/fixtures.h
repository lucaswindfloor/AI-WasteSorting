#pragma once

#include "../m02_calibration/fixtures.h"
#include "m03_inference/inference_service.h"

namespace waste::m03::test {

using namespace std::chrono_literals;
using m02::test::check;

inline Manifest manifest(Route route = Route::Rgb, std::string version = "1") {
    Manifest m;
    m.identity = {{"fixture", std::move(version)}, "fixture-digest", "fake", "fp32", "processing-1"};
    m.route = route;
    if (route != Route::Hsi) {
        m.rgb = Channels{"RGB8", {}, m02::SpectralLevel::Raw, {}, {1, 0, 0}, {0.5f, 1, 1}};
    }
    if (route != Route::Rgb) {
        m.hsi = Channels{"U16LE", {"500nm"}, m02::SpectralLevel::DarkWhiteCorrected, "white1", {0}, {1}};
    }
    m.window = {4, 2, 1, 16, 10ms, true, 0.5f};
    m.raster = {0, 0, 4, 2, 4, 2, 0, 1, 0, 1, -3};
    const auto kind = route == Route::Rgb ? EvidenceKind::Contour :
                      route == Route::Hsi ? EvidenceKind::Material : EvidenceKind::JointInstance;
    m.labels = {{route == Route::Rgb ? "object" : "PET", kind}};
    m.threshold = 0.5f;
    m.max_candidates = 4;
    return m;
}

inline m02::AlignedTrigger aligned_fixture(std::uint64_t id, bool missing_hsi = false) {
    using m01::Quality;
    const auto event = m02::test::trigger(id, 10ms, 1000);
    const auto time = m02::TimeEstimate{10ms, 1ns};
    const auto count = m02::CountEstimate{7, 1000, 0};
    const auto make = [&](m01::Source source) {
        const bool rgb = source == m01::Source::Rgb;
        const auto bytes = rgb ? std::vector<std::byte>{std::byte{5}, std::byte{6}, std::byte{7},
                                                       std::byte{9}, std::byte{10}, std::byte{11}}
                               : std::vector<std::byte>(4);
        m02::AlignedObservation row;
        row.source = m02::test::row(source, rgb ? 12 : 13, rgb ? "rgb" : "hsi", 10ms, bytes, event.key);
        row.source.header.id.frame = id + (rgb ? 100 : 200);
        row.source_quality = Quality::Valid;
        row.time = row.reference_time = time;
        row.count = count;
        row.shift = m02::BeltShift{0, 0, count, count};
        row.measured = {{Quality::Valid, {0, 406, 0}, 0.1}, {Quality::Valid, {0, 410, 0}, 0.1}};
        row.aligned = row.measured;
        if (!rgb) {
            m02::SpectralRow spectrum;
            spectrum.level = m02::SpectralLevel::DarkWhiteCorrected;
            spectrum.reference_version = "white1";
            spectrum.samples = {0.5, 0.75};
            spectrum.quality = {Quality::Valid, Quality::Valid};
            row.spectrum = std::move(spectrum);
        }
        return row;
    };
    m02::AlignedTrigger result;
    result.source.event = event;
    result.reference_time = time;
    result.reference_count = count;
    result.rgb = make(m01::Source::Rgb);
    result.source.rgb = result.rgb->source;
    result.source.rgb_quality = Quality::Valid;
    if (!missing_hsi) {
        result.hsi = make(m01::Source::Hsi);
        result.source.hsi = result.hsi->source;
        result.source.hsi_quality = Quality::Valid;
        result.common_coverage = {{0, 0}, {1, 1}};
    }
    return result;
}

inline std::shared_ptr<m02::Result> segment(const m01::Config& c, const m02::Snapshot& s,
                                           std::vector<m02::AlignedTrigger> rows) {
    auto result = std::make_shared<m02::Result>();
    m01::Segment source;
    source.run_id = c.run_id;
    source.generation = c.generation;
    source.sequence = rows.empty() ? 0 : rows.front().source.event.key.id;
    source.config = std::make_shared<m01::Config>(c);
    result->source = std::make_shared<m01::Segment>(std::move(source));
    result->snapshot = std::make_shared<m02::Snapshot>(s);
    result->triggers = std::move(rows);
    return result;
}

inline Window input_window(const Manifest& m, bool missing_hsi = false) {
    const auto c = m02::test::acquisition_config();
    std::vector<m02::AlignedTrigger> rows;
    for (std::size_t i = 0; i < m.window.rows; ++i) {
        rows.push_back(aligned_fixture(2147 + i, i == 1 && missing_hsi));
    }
    auto input = segment(c, m02::test::calibration(c), std::move(rows));
    Window result{{{c.run_id, c.generation}, m.identity.key, 2147, 2147 + m.window.rows - 1},
                  {}, 0ms, 1ms, 50ms};
    for (std::size_t i = 0; i < m.window.rows; ++i) {
        result.rows.emplace_back(InputRow{input, i, 0ms, 50ms});
    }
    return result;
}

void manifest_test();
void builder_test();
void preprocessor_test();
void projection_test();

}  // namespace waste::m03::test
