#include "model_manifest.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace waste::m03 {
namespace {

// Structural limits bound allocations even for an incorrectly configured package.
constexpr std::size_t max_axis = 4096;
constexpr std::size_t max_native_columns = 8192;
constexpr std::size_t max_pixels = 1024 * 1024;
constexpr std::size_t max_values = 4 * 1024 * 1024;
constexpr std::size_t max_projection_work = 8 * 1024 * 1024;

bool valid_channels(const Channels& c, std::size_t count) {
    return count && count <= 1024 && c.offset.size() == count && c.scale.size() == count &&
           std::ranges::all_of(c.offset, [](float x) { return std::isfinite(x); }) &&
           std::ranges::all_of(c.scale, [](float x) { return std::isfinite(x) && x != 0; });
}

bool valid_hsi(const Channels& c) {
    const std::set<std::string> unique(c.bands.begin(), c.bands.end());
    return valid_channels(c, c.bands.size()) && unique.size() == c.bands.size() && !unique.contains("") &&
           (c.encoding == "U8" || c.encoding == "U16LE" || c.encoding == "F32LE") &&
           (c.spectral_level == m02::SpectralLevel::Raw ? c.spectral_reference.empty()
                                                       : !c.spectral_reference.empty());
}

}  // namespace

std::size_t Raster::height() const { return resized_rows + pad_top + pad_bottom; }
std::size_t Raster::width() const { return resized_columns + pad_left + pad_right; }

void validate(const Manifest& m) {
    const auto& i = m.identity;
    if (i.key.package.empty() || i.key.version.empty() || i.artifact_digest.empty() ||
        i.backend.empty() || i.precision.empty() || i.processing_version.empty()) {
        throw std::invalid_argument("model identity or runtime is incomplete");
    }
    if ((m.route != Route::Rgb && m.route != Route::Hsi && m.route != Route::Joint) ||
        m.rgb.has_value() != (m.route != Route::Hsi) || m.hsi.has_value() != (m.route != Route::Rgb) ||
        (m.rgb && (m.rgb->encoding != "RGB8" || !m.rgb->bands.empty() || !valid_channels(*m.rgb, 3))) ||
        (m.hsi && !valid_hsi(*m.hsi))) {
        throw std::invalid_argument("unsupported or incomplete model input contract");
    }
    const auto& w = m.window;
    if (!w.rows || w.rows > max_axis || !w.step || w.step > w.rows ||
        w.max_missing_rows >= w.rows || w.max_buffered_rows < w.rows ||
        w.max_buffered_rows > 4 * max_axis || w.max_wait <= Tick::zero() ||
        !std::isfinite(w.min_valid_fraction) || w.min_valid_fraction <= 0 || w.min_valid_fraction > 1 ||
        (!w.accepts_mask && (w.max_missing_rows || w.min_valid_fraction != 1))) {
        throw std::invalid_argument("invalid window or mask policy");
    }
    const auto& r = m.raster;
    if (!r.rows || !r.columns || r.top >= w.rows || r.rows > w.rows - r.top ||
        r.left > max_native_columns || r.columns > max_native_columns || !r.resized_rows || !r.resized_columns ||
        r.resized_rows > max_axis || r.resized_columns > max_axis ||
        r.pad_top > max_axis || r.pad_left > max_axis || r.pad_bottom > max_axis || r.pad_right > max_axis ||
        r.height() > max_axis || r.width() > max_axis || !std::isfinite(r.fill)) {
        throw std::invalid_argument("invalid crop, resize, or padding");
    }
    const auto pixels = r.height() * r.width();
    const auto channels = (m.rgb ? std::size_t{3} : 0) + (m.hsi ? m.hsi->bands.size() : 0);
    // RGB reads only resized sample positions; the source-size limit for joint mapping stays smaller.
    const auto max_source_pixels = m.route == Route::Rgb ? 4 * max_pixels : max_pixels;
    if (r.rows > max_source_pixels / r.columns || pixels > max_pixels || pixels > max_values / channels || !m.max_candidates ||
        m.max_candidates > 1024 || pixels > max_projection_work / m.max_candidates || m.labels.empty() ||
        m.labels.size() > 1024 || !std::isfinite(m.threshold) ||
        (m.score_kind != ScoreKind::Probability && m.score_kind != ScoreKind::Score) ||
        (m.score_kind == ScoreKind::Probability && (m.threshold < 0 || m.threshold > 1))) {
        throw std::invalid_argument("invalid output contract or model size");
    }
    std::set<std::string> names;
    for (const auto& label : m.labels) {
        if (label.name.empty() || !names.insert(label.name).second ||
            (label.kind != EvidenceKind::Contour && label.kind != EvidenceKind::Material &&
             label.kind != EvidenceKind::JointInstance) ||
            (m.route == Route::Rgb && label.kind != EvidenceKind::Contour) ||
            (m.route == Route::Hsi && label.kind != EvidenceKind::Material)) {
            throw std::invalid_argument("invalid model label semantics");
        }
    }
}

void verify_compatibility(const Manifest& m, const m01::Config& c, const m02::Snapshot& s) {
    const bool compatible_routing = c.routing == m01::Routing::SharedTrigger ||
        (c.routing == m01::Routing::RgbTriggered && m.route == Route::Rgb);
    if (!compatible_routing || !c.contiguous_trigger_ids) {
        throw std::invalid_argument("M03 grid requires verified contiguous trigger IDs for its route");
    }
    const auto& grid = m.rgb ? c.rgb.format : c.hsi.format;
    if (m.raster.left >= grid.width || m.raster.columns > grid.width - m.raster.left ||
        (m.rgb && (m.rgb->encoding != c.rgb.format.encoding || c.rgb.format.transport_layout != "packed")) ||
        (m.hsi && (m.hsi->encoding != c.hsi.format.encoding || c.hsi.format.transport_layout != "packed" ||
                   m.hsi->bands != c.hsi.format.bands))) {
        throw std::invalid_argument("model crop, format or ordered bands mismatch");
    }
    if (m.hsi) {
        if (m.hsi->spectral_level == m02::SpectralLevel::DarkWhiteCorrected) {
            if (!s.spectral || m.hsi->spectral_reference != s.versions.spectral_reference ||
                m.hsi->bands != s.spectral->bands) {
                throw std::invalid_argument("model spectral reference mismatch");
            }
        } else if (s.spectral && s.spectral->input_level == m02::SpectralLevel::DarkWhiteCorrected) {
            throw std::invalid_argument("raw HSI required but source already corrected");
        }
    }
}

}  // namespace waste::m03
