#include "calibration_snapshot.h"
#include "coordinate_transform.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace waste::m02 {
namespace {

bool nonnegative(double value) {
    return std::isfinite(value) && value >= 0;
}

bool positive(double value) {
    return nonnegative(value) && value > 0;
}

const DeviceKey* device(const Snapshot& snapshot, m01::Source source) {
    const auto found = std::ranges::find(snapshot.devices, source, &DeviceKey::source);
    return found == snapshot.devices.end() ? nullptr : &*found;
}

bool valid_scan(const ScanCalibration& scan, std::size_t width, double height) {
    if (scan.layers.empty() || !width || !nonnegative(scan.residual_mm) ||
        height < scan.layers.front().height_mm || height > scan.layers.back().height_mm) {
        return false;
    }
    std::optional<double> previous;
    for (const auto& layer : scan.layers) {
        if (!std::isfinite(layer.height_mm) || (previous && layer.height_mm <= *previous) ||
            layer.columns.size() != width ||
            (!layer.valid_columns.empty() && layer.valid_columns.size() != width) ||
            std::ranges::any_of(layer.columns, [&](auto point) {
                return !finite(point) || point.z != layer.height_mm;
            })) {
            return false;
        }
        previous = layer.height_mm;
    }
    return true;
}

void verify_clocks(const Snapshot& snapshot) {
    for (std::size_t i = 0; i < snapshot.clocks.size(); ++i) {
        const auto& clock = snapshot.clocks[i];
        const auto source = device(snapshot, clock.source);
        if (!source || clock.session != source->session || clock.domain.empty() ||
            clock.time_basis.empty() || clock.first_tick > clock.last_tick ||
            clock.host_anchor.count() < 0 || clock.error.count() < 0 || !positive(clock.ns_per_tick)) {
            throw std::invalid_argument("invalid clock mapping");
        }
        for (std::size_t j = 0; j < i; ++j) {
            const auto& other = snapshot.clocks[j];
            if (clock.source == other.source && clock.domain == other.domain &&
                clock.time_basis == other.time_basis && clock.first_tick <= other.last_tick &&
                other.first_tick <= clock.last_tick) {
                throw std::invalid_argument("overlapping clock mappings");
            }
        }
    }
    std::set<m01::Source> delay_sources;
    for (const auto& delay : snapshot.trigger_delays) {
        const auto source = device(snapshot, delay.source);
        if (!source || source->session != delay.session || delay.source == m01::Source::Sync ||
            delay.error.count() < 0 || !delay_sources.insert(delay.source).second) {
            throw std::invalid_argument("invalid trigger delay");
        }
    }
}

void verify_spectral(const Snapshot& snapshot, const DeviceKey& hsi) {
    if (!snapshot.spectral) {
        return;
    }
    const auto& spec = *snapshot.spectral;
    if (snapshot.versions.spectral_reference.empty() || spec.settings_id.empty() ||
        spec.bands.empty() || spec.bands != hsi.format.bands || !positive(spec.denominator_min) ||
        !positive(spec.saturation) || spec.environment_max_age.count() < 0 ||
        !std::isfinite(spec.temperature_min) || !std::isfinite(spec.temperature_max) ||
        spec.temperature_min > spec.temperature_max || !nonnegative(spec.light_min) ||
        !nonnegative(spec.light_max) || spec.light_min > spec.light_max) {
        throw std::invalid_argument("invalid spectral reference settings");
    }
    if (spec.input_level == SpectralLevel::Raw) {
        const auto columns = spec.reference_layout == ReferenceLayout::PerBand ? 1U : hsi.format.width;
        if (spec.dark.size() != spec.white.size() || spec.dark.size() % spec.bands.size() ||
            spec.dark.size() / spec.bands.size() != columns) {
            throw std::invalid_argument("spectral reference array size mismatch");
        }
    }
}

void verify(const Snapshot& s) {
    const auto rgb = device(s, m01::Source::Rgb);
    const auto hsi = device(s, m01::Source::Hsi);
    const auto sync = device(s, m01::Source::Sync);
    if (s.run_id.empty() || s.versions.geometry.empty() || s.versions.clock.empty() ||
        s.versions.encoder.empty() || !rgb || !hsi || !sync ||
        !std::isfinite(s.reference_height_mm) || !valid_scan(s.rgb, rgb->format.width, s.reference_height_mm) ||
        !valid_scan(s.hsi, hsi->format.width, s.reference_height_mm) ||
        s.max_exposure_separation.count() <= 0 || s.max_reference_separation.count() <= 0 || !positive(s.max_pair_dx_mm) || !positive(s.max_pair_dy_mm)) {
        throw std::invalid_argument("invalid calibration identity, scan, or tolerance");
    }
    std::set<m01::Source> sources;
    for (const auto& key : s.devices) {
        if (key.device_id.empty() || !sources.insert(key.source).second) {
            throw std::invalid_argument("missing or duplicate calibration device");
        }
    }
    const auto& e = s.encoder;
    if (!positive(std::abs(e.signed_mm_per_count)) || !nonnegative(e.lock_error_counts) ||
        !nonnegative(e.native_error_counts) || !nonnegative(e.interpolation_error_counts) ||
        e.max_sample_gap.count() <= 0 || !positive(e.max_count_rate_per_second) ||
        !positive(e.max_displacement_mm)) {
        throw std::invalid_argument("invalid encoder calibration");
    }
    for (auto source : e.verified_native_sources) {
        if (source == m01::Source::Sync || !device(s, source)) {
            throw std::invalid_argument("invalid native count source");
        }
    }
    if (s.depth) {
        const auto key = device(s, m01::Source::E05);
        const auto& d = *s.depth;
        if (!key || key->format.units != "mm" || !valid_transform(d.to_belt) ||
            !valid_bounds(d.source_bounds) || !valid_bounds(d.belt_bounds) ||
            !nonnegative(d.residual_mm) ||
            (key->format.encoding == "HEIGHT_F32LE" &&
             (!finite(d.grid_origin) || !positive(std::abs(d.row_step_mm)) ||
              !positive(std::abs(d.column_step_mm))))) {
            throw std::invalid_argument("invalid depth calibration");
        }
    }
    if (s.robot && (s.robot->tool_id.empty() || !valid_transform(s.robot->base_from_belt) ||
        !valid_transform(s.robot->flange_from_tcp) || !valid_bounds(s.robot->belt_bounds) ||
        !nonnegative(s.robot->residual_mm))) {
        throw std::invalid_argument("invalid robot or tool calibration");
    }
    verify_clocks(s);
    verify_spectral(s, *hsi);
}

}  // namespace

void CalibrationSnapshots::install(Snapshot snapshot) {
    verify(snapshot);
    auto key = Key{snapshot.run_id, snapshot.generation};
    if (snapshots_.contains(key)) {
        throw std::invalid_argument("calibration generation is already registered");
    }
    snapshots_.emplace(std::move(key), std::make_shared<const Snapshot>(std::move(snapshot)));
}

std::shared_ptr<const Snapshot> CalibrationSnapshots::find(const std::string& run_id,
                                                            std::uint64_t generation) const {
    const auto found = snapshots_.find({run_id, generation});
    return found != snapshots_.end() ? found->second : nullptr;
}

void CalibrationSnapshots::retire(const std::string& run_id, std::uint64_t generation) {
    if (const auto found = snapshots_.find({run_id, generation}); found != snapshots_.end()) {
        found->second.reset();
    }
}

bool matches(const Snapshot& snapshot, const m01::Config& config) {
    const auto same = [&](m01::Source source, const m01::SourceConfig& installed) {
        const auto key = device(snapshot, source);
        return key && installed.settings_verified && key->device_id == installed.device_id &&
               key->session == installed.session && key->firmware == installed.firmware &&
               key->sdk == installed.sdk && key->format == installed.format;
    };
    return snapshot.run_id == config.run_id && snapshot.generation == config.generation &&
           snapshot.encoder.epoch == config.encoder_epoch && same(m01::Source::Sync, config.sync) &&
           same(m01::Source::Rgb, config.rgb) && same(m01::Source::Hsi, config.hsi) &&
           (config.e05 ? same(m01::Source::E05, *config.e05) : !device(snapshot, m01::Source::E05));
}

}  // namespace waste::m02
