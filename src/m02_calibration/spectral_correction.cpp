#include "spectral_correction.h"
#include "sample_bytes.h"

#include <cmath>
#include <limits>

namespace waste::m02 {
namespace {

bool suitable(const Environment* state, const Snapshot& snapshot,
              const SpectralCalibration& spec, const std::optional<TimeEstimate>& exposure) {
    if (!state || !exposure || state->run_id != snapshot.run_id ||
        state->generation != snapshot.generation || state->settings_id != spec.settings_id ||
        !state->reference_healthy || !std::isfinite(state->temperature) || !std::isfinite(state->light) ||
        state->temperature < spec.temperature_min || state->temperature > spec.temperature_max ||
        state->light < spec.light_min || state->light > spec.light_max ||
        state->measured.count() < 0 || exposure->error.count() < 0) {
        return false;
    }
    const auto age = static_cast<long double>(exposure->value.count()) - state->measured.count();
    return age >= exposure->error.count() &&
           age + exposure->error.count() <= spec.environment_max_age.count();
}

double read_sample(std::span<const std::byte> sample) {
    if (sample.size() == 1) {
        return octet(sample[0]);
    }
    if (sample.size() == 2) {
        return octet(sample[0]) | (octet(sample[1]) << 8);
    }
    return f32_le(sample);
}

}  // namespace

SpectralRow correct_spectrum(const Snapshot& snapshot, const m01::DataFormat& format,
                             const m01::Observation& observation,
                             const std::optional<TimeEstimate>& exposure,
                             const Environment* environment) {
    SpectralRow result;
    if (!snapshot.spectral) {
        result.reason = "no_spectral_reference";
        return result;
    }
    const auto& spec = *snapshot.spectral;
    result.level = spec.input_level;
    result.reference_version = snapshot.versions.spectral_reference;
    if (!suitable(environment, snapshot, spec, exposure)) {
        result.reason = "reference_conditions_unverified";
        return result;
    }
    const auto bands = spec.bands.size();
    const auto stride = format.encoding == "U8" ? 1U : format.encoding == "U16LE" ? 2U :
                        format.encoding == "F32LE" ? 4U : 0U;
    const auto bytes = byte_range(observation, observation.header.bytes);
    if (!stride || !bands || format.transport_layout != "packed" || format.bands != spec.bands ||
        !bytes || bytes->size() % stride || (bytes->size() / stride) % bands ||
        (bytes->size() / stride) / bands != format.width) {
        result.reason = "spectral_layout_mismatch";
        return result;
    }

    const auto count = bytes->size() / stride;
    const auto references = spec.reference_layout == ReferenceLayout::PerBand ? bands : count;
    if (spec.input_level == SpectralLevel::Raw &&
        (spec.dark.size() != references || spec.white.size() != references)) {
        result.reason = "spectral_reference_size_mismatch";
        return result;
    }
    std::optional<std::span<const std::byte>> mask;
    if (observation.header.valid_mask) {
        mask = byte_range(observation, *observation.header.valid_mask);
        if (!mask || mask->size() != count) {
            result.reason = "spectral_mask_layout_mismatch";
            return result;
        }
    }
    result.level = SpectralLevel::DarkWhiteCorrected;
    result.samples.resize(count, std::numeric_limits<double>::quiet_NaN());
    result.quality.resize(count, Quality::Invalid);
    result.sample_issues.resize(count, SpectralIssue::NonFinite);
    for (std::size_t i = 0; i < count; ++i) {
        if (mask && (*mask)[i] == std::byte{0}) {
            result.quality[i] = Quality::Missing;
            result.sample_issues[i] = SpectralIssue::Masked;
            continue;
        }
        const auto sample = read_sample(bytes->subspan(i * stride, stride));
        if (!std::isfinite(sample)) {
            continue;
        }
        if (spec.input_level == SpectralLevel::DarkWhiteCorrected) {
            result.samples[i] = sample;
            result.quality[i] = Quality::Valid;
            result.sample_issues[i] = SpectralIssue::None;
            continue;
        }
        const auto ref = spec.reference_layout == ReferenceLayout::PerBand ? i % bands : i;
        const auto dark = spec.dark[ref];
        const auto white = spec.white[ref];
        const auto denominator = white - dark;
        if (!std::isfinite(denominator) || denominator < spec.denominator_min) {
            result.sample_issues[i] = SpectralIssue::InvalidReference;
            continue;
        }
        if (sample >= spec.saturation || dark >= spec.saturation || white >= spec.saturation) {
            result.sample_issues[i] = SpectralIssue::Saturated;
            continue;
        }
        const auto corrected = (sample - dark) / denominator;
        if (std::isfinite(corrected)) {
            result.samples[i] = corrected;
            result.quality[i] = Quality::Valid;
            result.sample_issues[i] = SpectralIssue::None;
        }
    }
    return result;
}

}  // namespace waste::m02
