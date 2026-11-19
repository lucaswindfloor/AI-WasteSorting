#include "spatial_alignment.h"
#include "coordinate_transform.h"
#include "sample_bytes.h"
#include "time_alignment.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace waste::m02 {

std::vector<ProjectedPoint> project_scan(const ScanCalibration& scan, double height_mm) {
    if (scan.layers.empty() || !std::isfinite(height_mm) ||
        height_mm < scan.layers.front().height_mm || height_mm > scan.layers.back().height_mm) {
        return {};
    }
    const auto above = std::ranges::lower_bound(scan.layers, height_mm, {}, &ScanLayer::height_mm);
    const auto valid_column = [](const ScanLayer& layer, std::size_t index) {
        return layer.valid_columns.empty() || layer.valid_columns[index] != 0;
    };
    std::vector<ProjectedPoint> result;
    result.reserve(above->columns.size());
    if (above->height_mm == height_mm) {
        for (std::size_t i = 0; i < above->columns.size(); ++i) {
            result.push_back({valid_column(*above, i) ? Quality::Valid : Quality::Invalid,
                              above->columns[i], scan.residual_mm});
        }
    } else {
        const auto& below = *(above - 1);
        const auto fraction = (height_mm - below.height_mm) / (above->height_mm - below.height_mm);
        for (std::size_t i = 0; i < above->columns.size(); ++i) {
            const auto low = below.columns[i];
            const auto high = above->columns[i];
            const auto quality = valid_column(*above, i) && valid_column(below, i)
                                     ? Quality::Valid : Quality::Invalid;
            result.push_back({quality, {std::lerp(low.x, high.x, fraction),
                                              std::lerp(low.y, high.y, fraction), height_mm},
                              scan.residual_mm});
        }
    }
    return result;
}

std::vector<ProjectedPoint> project_depth(const Snapshot& snapshot, const m01::DataFormat& format,
                                          const m01::Observation& source) {
    if (!snapshot.depth || format.units != "mm" || format.transport_layout != "packed" ||
        (format.encoding != "XYZ_F32LE" && format.encoding != "HEIGHT_F32LE")) {
        return {};
    }
    const bool xyz = format.encoding == "XYZ_F32LE";
    const std::size_t stride = xyz ? 12 : 4;
    const auto bytes = byte_range(source, source.header.bytes);
    if (!bytes || bytes->size() % stride || bytes->size() / stride != format.width) {
        return {};
    }
    std::optional<std::span<const std::byte>> mask;
    if (source.header.valid_mask) {
        mask = byte_range(source, *source.header.valid_mask);
        if (!mask || mask->size() != format.width) {
            return {};
        }
    }
    const auto& depth = *snapshot.depth;
    std::vector<ProjectedPoint> points(format.width);
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (mask && (*mask)[i] == std::byte{0}) {
            continue;
        }
        const auto sample = bytes->subspan(i * stride, stride);
        const auto local = xyz ? Point{f32_le(sample), f32_le(sample.subspan(4)),
                                      f32_le(sample.subspan(8))}
                               : Point{depth.grid_origin.x + source.header.id.row * depth.row_step_mm,
                                       depth.grid_origin.y + i * depth.column_step_mm,
                                       depth.grid_origin.z + f32_le(sample)};
        points[i] = transform_point(snapshot, Frame::Depth, Frame::Belt, local)
                        .value_or(ProjectedPoint{Quality::Invalid, {}, 0});
    }
    return points;
}

std::vector<ProjectedPoint> shift_points(std::span<const ProjectedPoint> measured, const BeltShift& shift) {
    std::vector<ProjectedPoint> result(measured.begin(), measured.end());
    for (auto& point : result) {
        if (point.quality == Quality::Valid) {
            point.position.x += shift.mm;
            point.error_mm += shift.error_mm;
            if (!finite(point.position) || !std::isfinite(point.error_mm)) {
                point = {Quality::Invalid, {}, 0};
            }
        }
    }
    return result;
}

std::vector<Coverage> overlap(const Snapshot& snapshot, const AlignedObservation& rgb,
                              const AlignedObservation& hsi) {
    if (!rgb.time || !hsi.time || !within_time(*rgb.time, *hsi.time, snapshot.max_exposure_separation)) {
        return {};
    }
    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < hsi.aligned.size(); ++i) {
        if (hsi.aligned[i].quality == Quality::Valid) {
            candidates.push_back(i);
        }
    }
    const auto lateral = [&](std::size_t i) { return hsi.aligned[i].position.y; };
    std::ranges::sort(candidates, [&](auto a, auto b) {
        return lateral(a) == lateral(b) ? a < b : lateral(a) < lateral(b);
    });

    std::vector<Coverage> result;
    for (std::size_t i = 0; i < rgb.aligned.size(); ++i) {
        const auto& a = rgb.aligned[i];
        if (a.quality != Quality::Valid) {
            continue;
        }
        const auto start = std::ranges::lower_bound(candidates, a.position.y - snapshot.max_pair_dy_mm,
                                                    {}, lateral);
        auto best_distance = std::numeric_limits<double>::infinity();
        std::optional<std::size_t> best;
        for (auto candidate = start; candidate != candidates.end() &&
             lateral(*candidate) <= a.position.y + snapshot.max_pair_dy_mm; ++candidate) {
            const auto& b = hsi.aligned[*candidate];
            const auto dx = std::abs(a.position.x - b.position.x);
            const auto dy = std::abs(a.position.y - b.position.y);
            const auto error = a.error_mm + b.error_mm;
            if (dx + error <= snapshot.max_pair_dx_mm && dy + error <= snapshot.max_pair_dy_mm &&
                dx + dy < best_distance) {
                best_distance = dx + dy;
                best = *candidate;
            }
        }
        if (best) {
            result.push_back({i, *best});
        }
    }
    return result;
}

}  // namespace waste::m02
