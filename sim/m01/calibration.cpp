#include "calibration.h"

#include <algorithm>

namespace waste::m01sim {
namespace {
using namespace std::chrono_literals;

m02::ScanCalibration columns(unsigned width, double x, double belt_width) {
    m02::ScanLayer plane;
    for (unsigned i = 0; i < width; ++i) {
        plane.columns.push_back({x, (i + 0.5) * belt_width / width, 0});
    }
    return {{std::move(plane)}, 0.02};
}
}  // namespace

m01::Tick clock_offset(m01::Source source) {
    switch (source) {
    case m01::Source::Sync: return 0ns;
    case m01::Source::Rgb: return 71us;
    case m01::Source::Hsi: return 183us;
    case m01::Source::E05: return 427us;
    }
    return {};
}

m01::Tick hsi_exposure_delay(Scenario scenario, m01::Tick interval) {
    if (scenario == Scenario::CountInterpolation) return interval / 2;
    if (scenario == Scenario::ExposureDelay || scenario == Scenario::Compensated ||
        scenario == Scenario::ExposureLimit) return 1ms;
    return 0ns;
}

m01::Tick hsi_transport_delay(Scenario scenario) {
    return scenario == Scenario::TransportDelay ? 1ms : 0ns;
}

double hsi_scan_position(const Layout& layout, Scenario scenario, m01::Tick interval) {
    if (scenario == Scenario::Compensated || scenario == Scenario::ExposureLimit ||
        scenario == Scenario::CountInterpolation) {
        return layout.hsi_x + layout.pitch * hsi_exposure_delay(scenario, interval).count() / interval.count();
    }
    return layout.hsi_x;
}

bool hsi_has_native_count(Scenario scenario, unsigned trigger) {
    // The last row has no following latch. This also exercises the no-extrapolation path.
    return scenario != Scenario::CountInterpolation || (trigger != 100 && trigger != trigger_count);
}

m02::Snapshot calibration(const m01::Config& config, const Layout& layout, Scenario scenario, m01::Tick interval) {
    m02::Snapshot out;
    out.run_id = config.run_id;
    out.generation = config.generation;
    const auto version = ".g" + std::to_string(config.generation);
    const auto geometry = layout.length ? "sim-geometry-" + std::to_string(static_cast<unsigned>(layout.width)) + "mm"
                                        : "sim-geometry";
    out.versions = {geometry + version, "sim-clock" + version, "sim-encoder" + version,
                    "sim-dark-white" + version};
    const auto add = [&](m01::Source source, const m01::SourceConfig& device) {
        out.devices.push_back({source, device.device_id, device.firmware, device.sdk, device.session, device.format});
        if (source == m01::Source::Hsi && scenario == Scenario::ClockMissing) return;
        const bool sync = source == m01::Source::Sync;
        const auto anchor = clock_offset(source).count();
        out.clocks.push_back({source, device.session, sync ? "sim-encoder-clock" : device.device_id + "-clock",
            sync ? "trigger" : std::string(exposure_basis), anchor, anchor, m01::Tick(layout.length ? 10s : 3s).count(), 0ns, 1.0, 0ns});
    };
    add(m01::Source::Sync, config.sync);
    add(m01::Source::Rgb, config.rgb);
    add(m01::Source::Hsi, config.hsi);
    add(m01::Source::E05, *config.e05);
    out.encoder = {config.encoder_epoch, layout.pitch / layout.encoder_step(), 0.01, 0.5, 0.1,
                   5ms, 65000.0, 80.0, {m01::Source::Rgb, m01::Source::Hsi, m01::Source::E05}};
    out.rgb = columns(config.rgb.format.width, layout.rgb_x, layout.width);
    out.hsi = columns(config.hsi.format.width, hsi_scan_position(layout, scenario, interval), layout.width);
    const auto step = layout.width / config.e05->format.width;
    const m02::Bounds bounds{{0, 0, -1}, {std::max(400.0, layout.length), layout.width, 100}};
    out.depth = m02::DepthCalibration{{}, bounds, bounds, 0.02, {layout.depth_x, step / 2, 0}, layout.pitch, step};
    const auto& bands = config.hsi.format.bands;
    out.spectral = m02::SpectralCalibration{std::string(calibration_settings), m02::SpectralLevel::Raw,
        m02::ReferenceLayout::PerBand, bands, std::vector<double>(bands.size(), 100),
        std::vector<double>(bands.size(), 30100), 1, 65535, 15, 35, 1, 10, layout.length ? 10s : 2s};
    if (scenario == Scenario::InvalidWhite) {
        out.spectral->white[bands.size() / 2] = out.spectral->dark[bands.size() / 2];
    }
    out.max_exposure_separation = scenario == Scenario::ExposureLimit ? m01::Tick(500us) : m01::Tick(2ms);
    out.max_reference_separation = 20ms;
    out.max_pair_dx_mm = 0.25;
    out.max_pair_dy_mm = 2.30;
    return out;
}

std::string_view name(m02::CoverageStatus status) {
    switch (status) {
    case m02::CoverageStatus::MissingInput: return "missing_input";
    case m02::CoverageStatus::UnusableInput: return "unusable_input";
    case m02::CoverageStatus::UnverifiedTime: return "unverified_time";
    case m02::CoverageStatus::ExposureSeparation: return "exposure_separation";
    case m02::CoverageStatus::UnverifiedCount: return "unverified_count";
    case m02::CoverageStatus::ReferenceUnavailable: return "reference_unavailable";
    case m02::CoverageStatus::ProjectionUnavailable: return "projection_unavailable";
    case m02::CoverageStatus::OutOfCoverage: return "out_of_coverage";
    case m02::CoverageStatus::Available: return "available";
    case m02::CoverageStatus::IndependentRoute: return "independent_route";
    }
    return {};
}

}  // namespace waste::m01sim
