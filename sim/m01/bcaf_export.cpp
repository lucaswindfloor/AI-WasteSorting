#include "bcaf_export.h"
#include "m02_calibration/sample_bytes.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace waste::m01sim {
namespace {

// The production simulator projects both sensors on a shared, uniform, full-width scan.
bool matches_scan(const m02::AlignedObservation& row, unsigned columns, double x, double width) {
    if (row.source_quality != m01::Quality::Valid || !row.time || !row.count ||
        row.measured.size() != columns || row.source.header.valid_mask) {
        return false;
    }
    for (unsigned i = 0; i < columns; ++i) {
        const auto& p = row.measured[i];
        if (p.quality != m01::Quality::Valid || std::abs(p.position.x - x) > 1e-6 ||
            std::abs(p.position.y - (i + 0.5) * width / columns) > 1e-6) return false;
    }
    return true;
}

}  // namespace

BcafExport::BcafExport(const std::filesystem::path& output)
    : rgb_(output / "bcaf-rgb.u8", std::ios::binary),
      hsi_(output / "bcaf-hsi.f32le", std::ios::binary) {
    static_assert(std::endian::native == std::endian::little &&
                  sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    if (!rgb_ || !hsi_) throw std::runtime_error("could not open BCAF capture files");
}

void BcafExport::append(const m02::Result& result, const Layout& layout) {
    const auto& config = *result.source->config;
    const auto rgb_width = config.rgb.format.width;
    const auto hsi_width = config.hsi.format.width;
    const auto values = hsi_width * config.hsi.format.bands.size();
    const std::vector<char> missing_rgb(3 * rgb_width, 0);
    for (const auto& trigger : result.triggers) {
        const auto id = trigger.source.event.key.id;
        rgb_.seekp(static_cast<std::streamoff>((id - 1) * missing_rgb.size()));
        if (!trigger.rgb) {
            rgb_.write(missing_rgb.data(), missing_rgb.size());
            continue;
        }
        const auto& row = *trigger.rgb;
        const auto bytes = m02::byte_range(row.source, row.source.header.bytes);
        const auto travel = row.count ? row.count->value * result.snapshot->encoder.signed_mm_per_count : 0;
        if (!matches_scan(row, rgb_width, layout.rgb_x, layout.width) ||
            !bytes || bytes->size() != missing_rgb.size() ||
            std::abs(travel - id * layout.pitch) > 1e-6) {
            throw std::runtime_error("BCAF export requires verified RGB geometry and exposure position");
        }
        rgb_.write(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    }
    spectrum_.resize(values + 1);
    for (const auto& row : result.independent) {
        if (row.source.header.id.source != m01::Source::Hsi) continue;
        if (!matches_scan(row, hsi_width, layout.hsi_x, layout.width) ||
            !row.spectrum || row.spectrum->level != m02::SpectralLevel::DarkWhiteCorrected ||
            row.spectrum->reference_version != result.snapshot->versions.spectral_reference ||
            row.spectrum->samples.size() != values || row.spectrum->quality.size() != values) {
            throw std::runtime_error("BCAF export requires verified HSI geometry and dark-white correction");
        }
        spectrum_[0] = static_cast<float>(row.count->value * result.snapshot->encoder.signed_mm_per_count +
                                          layout.rgb_x - layout.hsi_x);
        for (std::size_t i = 0; i < values; ++i) {
            spectrum_[1 + i] = row.spectrum->quality[i] == m01::Quality::Valid ?
                static_cast<float>(row.spectrum->samples[i]) : std::numeric_limits<float>::quiet_NaN();
        }
        hsi_.write(reinterpret_cast<const char*>(spectrum_.data()), spectrum_.size() * sizeof(float));
    }
    if (!rgb_ || !hsi_) throw std::runtime_error("could not write BCAF capture");
}

void BcafExport::finish() {
    rgb_.close();
    hsi_.close();
    if (!rgb_ || !hsi_) throw std::runtime_error("could not finish BCAF capture");
}

}  // namespace waste::m01sim
