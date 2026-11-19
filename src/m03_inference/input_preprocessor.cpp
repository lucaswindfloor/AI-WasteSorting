#include "input_preprocessor.h"
#include "m02_calibration/sample_bytes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace waste::m03 {
namespace {

struct PlaneRow {
    const m02::AlignedObservation* observation{};
    std::span<const std::byte> bytes, mask;
    std::size_t width{}, stride{}, channels{};
    bool corrected{};
};

std::size_t byte_stride(const std::string& encoding) {
    if (encoding == "F32LE") {
        return 4;
    }
    return encoding == "U16LE" ? 2 : 1;
}

PlaneRow plane(const std::optional<m02::AlignedObservation>& row, const Channels& channels,
               const m01::DataFormat& format, bool rgb) {
    if (!row || row->source_quality != m01::Quality::Valid || !row->time) {
        return {};
    }
    const auto count = rgb ? std::size_t{3} : channels.bands.size();
    const auto stride = byte_stride(channels.encoding);
    const auto corrected = !rgb && channels.spectral_level == m02::SpectralLevel::DarkWhiteCorrected;
    PlaneRow p{&*row, {}, {}, format.width, stride, count, corrected};
    if (corrected) {
        if (!row->spectrum || row->spectrum->level != channels.spectral_level ||
            row->spectrum->reference_version != channels.spectral_reference ||
            row->spectrum->samples.size() != p.width * count ||
            row->spectrum->quality.size() != p.width * count) {
            return {};
        }
    } else {
        const auto raw = m02::byte_range(row->source, row->source.header.bytes);
        if (!raw || raw->size() != p.width * stride * count) {
            return {};
        }
        p.bytes = *raw;
    }
    if (row->source.header.valid_mask) {
        const auto mask = m02::byte_range(row->source, *row->source.header.valid_mask);
        if (!mask || mask->size() != p.width * (rgb ? 1 : count)) {
            return {};
        }
        p.mask = *mask;
    }
    return p;
}

std::optional<float> sample(const PlaneRow& row, const Channels& spec,
                            std::size_t column, std::size_t channel, bool rgb) {
    if (!row.mask.empty() && row.mask[rgb ? column : column * row.channels + channel] == std::byte{0}) {
        return {};
    }
    const auto index = column * row.channels + channel;
    double value;
    if (row.corrected) {
        const auto& spectrum = *row.observation->spectrum;
        if (spectrum.quality[index] != m01::Quality::Valid) {
            return {};
        }
        value = spectrum.samples[index];
    } else {
        const auto bytes = row.bytes.subspan(index * row.stride, row.stride);
        if (row.stride == 4) {
            value = m02::f32_le(bytes);
        } else {
            value = row.stride == 2 ? (m02::octet(bytes[0]) | m02::octet(bytes[1]) << 8)
                                    : m02::octet(bytes[0]);
        }
    }
    const auto normalized = (value - spec.offset[channel]) * spec.scale[channel];
    if (!std::isfinite(normalized) || std::abs(normalized) > std::numeric_limits<float>::max()) {
        return {};
    }
    return static_cast<float>(normalized);
}

std::optional<SampleLocation> location(const PlaneRow& plane, const InputRow& input,
                                        std::size_t column, bool joint) {
    if (!plane.observation || column >= plane.width) {
        return {};
    }
    const auto& row = *plane.observation;
    const bool aligned = row.shift && row.reference_time && column < row.aligned.size();
    const auto& points = aligned ? row.aligned : row.measured;
    if ((joint && !aligned) || column >= points.size() || points[column].quality != m01::Quality::Valid) {
        return {};
    }
    const auto& raw = row.source.header;
    const auto& trigger = input.owner->triggers[input.trigger_index].source.event.key;
    return SampleLocation{{input.owner->source->sequence, trigger, raw.id, raw.bytes, column},
                          points[column], aligned ? *row.reference_time : *row.time,
                          aligned ? std::optional(row.shift->to) : row.count,
                          aligned ? row.shift : std::nullopt, row.assumed_height_mm};
}

bool fill_pixel(Tensor& tensor, const Channels& channels, const PlaneRow& plane,
                std::size_t pixel, std::size_t column, std::size_t& valid_values) {
    bool any = false;
    for (std::size_t channel = 0; channel < plane.channels; ++channel) {
        if (const auto value = sample(plane, channels, column, channel, tensor.source == m01::Source::Rgb)) {
            const auto index = pixel * plane.channels + channel;
            tensor.values[index] = *value;
            tensor.valid[index] = 1;
            ++valid_values;
            any = true;
        }
    }
    return any;
}

std::size_t nearest(std::size_t output, std::size_t input_length, std::size_t output_length) {
    return ((2 * output + 1) * input_length) / (2 * output_length);
}

struct RasterRow {
    PlaneRow rgb, hsi;
    std::vector<std::optional<std::size_t>> hsi_column;
};

RasterRow read_row(const Manifest& m, const InputRow& input) {
    const auto& group = input.owner->triggers[input.trigger_index];
    const auto& format = *input.owner->source->config;
    RasterRow r;
    if (m.rgb) {
        r.rgb = plane(group.rgb, *m.rgb, format.rgb.format, true);
    }
    if (m.hsi) {
        r.hsi = plane(group.hsi, *m.hsi, format.hsi.format, false);
    }
    if (m.route == Route::Joint && r.rgb.observation && r.hsi.observation) {
        r.hsi_column.resize(m.raster.columns);
        for (const auto& match : group.common_coverage) {
            if (match.rgb_column >= m.raster.left && match.rgb_column - m.raster.left < m.raster.columns &&
                match.hsi_column < r.hsi.width) {
                r.hsi_column[match.rgb_column - m.raster.left] = match.hsi_column;
            }
        }
    }
    return r;
}

}  // namespace

std::size_t input_bytes(const Manifest& manifest) {
    const auto pixels = manifest.raster.height() * manifest.raster.width();
    const auto channels = (manifest.rgb ? std::size_t{3} : 0) +
                          (manifest.hsi ? manifest.hsi->bands.size() : 0);
    return sizeof(PreparedInput) + 2 * sizeof(Tensor) +
           pixels * (sizeof(PixelMap) + channels * (sizeof(float) + sizeof(std::uint8_t)));
}

Preparation prepare(const Manifest& m, const Window& window) {
    if (window.rows.size() != m.window.rows) {
        return {{}, "wrong_window_shape"};
    }
    const auto& r = m.raster;
    auto input = std::make_shared<PreparedInput>();
    input->height = r.height();
    input->width = r.width();
    const auto pixels = input->height * input->width;
    input->tensors.reserve(2);
    const auto add_tensor = [&](m01::Source source, std::size_t count) {
        input->tensors.push_back({source, input->height, input->width, count,
                                 std::vector<float>(pixels * count, r.fill),
                                 std::vector<std::uint8_t>(pixels * count, 0)});
    };
    if (m.rgb) {
        add_tensor(m01::Source::Rgb, 3);
    }
    if (m.hsi) {
        add_tensor(m01::Source::Hsi, m.hsi->bands.size());
    }
    input->source_map.resize(pixels);
    auto* rgb = m.rgb ? &input->tensors.front() : nullptr;
    auto* hsi = m.hsi ? &input->tensors.back() : nullptr;

    std::vector<RasterRow> source_rows(r.rows);
    std::size_t missing = 0;
    for (std::size_t i = 0; i < r.rows; ++i) {
        if (const auto& slot = window.rows[r.top + i]) {
            source_rows[i] = read_row(m, *slot);
        }
        const auto& row = source_rows[i];
        if ((rgb && !row.rgb.observation) || (hsi && !row.hsi.observation) ||
            (m.route == Route::Joint &&
             std::ranges::none_of(row.hsi_column, [](const auto& index) { return index.has_value(); }))) {
            ++missing;
        }
    }
    if (missing > m.window.max_missing_rows) {
        return {{}, "too_many_missing_rows"};
    }

    std::size_t valid_rgb = 0, valid_hsi = 0;
    for (std::size_t y = 0; y < r.resized_rows; ++y) {
        const auto source_y = nearest(y, r.rows, r.resized_rows);
        const auto& slot = window.rows[r.top + source_y];
        if (!slot) {
            continue;
        }
        const auto& row = source_rows[source_y];
        for (std::size_t x = 0; x < r.resized_columns; ++x) {
            const auto column = nearest(x, r.columns, r.resized_columns);
            const auto pixel = (y + r.pad_top) * input->width + x + r.pad_left;
            const auto joint = m.route == Route::Joint;
            const auto hsi_column = joint ? (column < row.hsi_column.size() ? row.hsi_column[column] : std::nullopt)
                                          : std::optional(column + r.left);
            auto rgb_at = rgb ? location(row.rgb, *slot, column + r.left, joint) : std::nullopt;
            auto hsi_at = hsi && hsi_column ? location(row.hsi, *slot, *hsi_column, joint) : std::nullopt;
            if (joint && (!rgb_at || !hsi_at)) {
                continue;
            }
            if (rgb_at && fill_pixel(*rgb, *m.rgb, row.rgb, pixel, column + r.left, valid_rgb)) {
                input->source_map[pixel].rgb = std::move(rgb_at);
            }
            if (hsi_at && fill_pixel(*hsi, *m.hsi, row.hsi, pixel, *hsi_column, valid_hsi)) {
                input->source_map[pixel].hsi = std::move(hsi_at);
            }
        }
    }
    const auto inner = r.resized_rows * r.resized_columns;
    const auto enough = [&](const Tensor* tensor, std::size_t valid) {
        const auto total = tensor ? inner * tensor->channels : 0;
        return !tensor || (valid > 0 && static_cast<double>(valid) >= static_cast<double>(m.window.min_valid_fraction) * total);
    };
    if (!enough(rgb, valid_rgb) || !enough(hsi, valid_hsi)) {
        return {{}, "insufficient_valid_samples"};
    }

    input->retained_bytes = sizeof(PreparedInput) + input->source_map.capacity() * sizeof(PixelMap) +
                            input->tensors.capacity() * sizeof(Tensor);
    for (const auto& tensor : input->tensors) {
        input->retained_bytes += tensor.values.capacity() * sizeof(float) + tensor.valid.capacity();
    }
    return {std::move(input), {}};
}

}  // namespace waste::m03
