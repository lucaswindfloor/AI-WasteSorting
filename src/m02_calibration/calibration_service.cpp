#include "calibration_service.h"
#include "encoder_reference.h"
#include "sample_bytes.h"
#include "spatial_alignment.h"
#include "spectral_correction.h"
#include "time_alignment.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace waste::m02 {
namespace {

bool current(const Snapshot& snapshot, const m01::Config& config, const m01::TriggerEvent& event) {
    return event.key.sync_session == config.sync.session && event.key.encoder_epoch == snapshot.encoder.epoch;
}

std::vector<CountSample> timeline(const Snapshot& snapshot, const m01::Segment& segment,
                                   const SyncWindow* window) {
    std::vector<CountSample> samples;
    std::map<m01::TriggerKey, const m01::TriggerEvent*> seen;
    bool conflict = false;
    const auto add = [&](const m01::TriggerEvent& event) {
        if (current(snapshot, *segment.config, event)) {
            const auto [entry, inserted] = seen.emplace(event.key, &event);
            if (!inserted) {
                conflict |= *entry->second != event;
            } else if (const auto time = sync_time(snapshot, event)) {
                samples.push_back({event.key.encoder_epoch, event.encoder_count, *time, event.direction});
            }
        }
    };
    for (const auto& item : segment.items) {
        if (const auto* trigger = std::get_if<m01::TriggerRecord>(&item)) {
            add(trigger->event);
        }
    }
    if (window && window->run_id == segment.run_id && window->generation == segment.generation) {
        for (const auto& event : window->events) {
            add(event);
        }
    }
    if (conflict) {
        return {};
    }
    std::ranges::sort(samples, {}, [](const CountSample& sample) { return sample.time.value; });
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const auto& a = samples[i - 1];
        const auto& b = samples[i];
        if (a.time.value == b.time.value && (a.count != b.count || a.time.error != b.time.error ||
                                                 a.direction != b.direction)) {
            return {};
        }
    }
    const auto duplicates = std::ranges::unique(samples, {}, [](const CountSample& sample) {
        return sample.time.value;
    });
    samples.erase(duplicates.begin(), duplicates.end());
    return samples;
}

const m01::SourceConfig* source_settings(const m01::Config& config, m01::Source source) {
    switch (source) {
    case m01::Source::Rgb: return &config.rgb;
    case m01::Source::Hsi: return &config.hsi;
    case m01::Source::E05: return config.e05 ? &*config.e05 : nullptr;
    case m01::Source::Sync: return nullptr;
    }
    return nullptr;
}

void apply_scan_mask(std::vector<ProjectedPoint>& geometry, const m01::Observation& row,
                     std::size_t values_per_column) {
    if (!row.header.valid_mask) {
        return;
    }
    const auto mask = byte_range(row, *row.header.valid_mask);
    if (!mask || !values_per_column || mask->size() % values_per_column ||
        mask->size() / values_per_column != geometry.size()) {
        geometry.clear();
        return;
    }
    for (std::size_t i = 0; i < geometry.size(); ++i) {
        const auto column = mask->subspan(i * values_per_column, values_per_column);
        if (std::ranges::all_of(column, [](auto value) { return value == std::byte{0}; })) {
            geometry[i].quality = Quality::Missing;
        }
    }
}

template<class Value>
TransformResult<Value> start_transform(const CalibrationSnapshots& snapshots,
                                        const CoordinateReference& reference) {
    TransformResult<Value> result;
    result.reference = reference;
    result.snapshot = snapshots.find(reference.run_id, reference.generation);
    if (!result.snapshot) {
        result.issue = TransformIssue::UnavailableGeneration;
    } else if (result.snapshot->versions.geometry != reference.geometry_version) {
        result.issue = TransformIssue::GeometryVersionMismatch;
    } else if (reference.count.epoch != result.snapshot->encoder.epoch ||
               reference.time.count() < 0 || !std::isfinite(reference.count.value) ||
               !std::isfinite(reference.count.error_counts) || reference.count.error_counts < 0) {
        result.issue = TransformIssue::InvalidReference;
    }
    return result;
}

AlignedObservation align(const Snapshot& snapshot, const m01::Config& config,
                         const m01::Observation& row, Quality quality,
                         std::span<const CountSample> samples,
                         const m01::TriggerRecord* trigger,
                         const std::optional<CountEstimate>& reference,
                         const std::optional<TimeEstimate>& reference_time,
                         const Environment* environment) {
    AlignedObservation result;
    result.source = row;
    if (quality == Quality::Valid) {
        quality = row.header.source_quality;
    }
    result.source_quality = quality;
    const auto* source = source_settings(config, row.header.id.source);
    const auto bytes = byte_range(row, row.header.bytes);
    if (!source || row.header.id.session != source->session ||
        row.header.id.row >= source->format.rows || !bytes || bytes->empty()) {
        result.source_quality = Quality::Invalid;
        result.issue = "invalid_observation";
        return result;
    }
    if (quality != Quality::Valid) {
        result.issue = "source_quality";
        return result;
    }

    result.time = observation_time(snapshot, row.header, trigger);
    result.count = observation_count(snapshot.encoder, row.header, result.time, samples);
    if (row.header.id.source == m01::Source::E05) {
        result.measured = project_depth(snapshot, source->format, row);
    } else {
        const bool rgb = row.header.id.source == m01::Source::Rgb;
        result.measured = project_scan(rgb ? snapshot.rgb : snapshot.hsi, snapshot.reference_height_mm);
        result.assumed_height_mm = snapshot.reference_height_mm;
        apply_scan_mask(result.measured, row, rgb ? 1 : source->format.bands.size());
        if (!rgb) {
            result.spectrum = correct_spectrum(snapshot, source->format, row, result.time, environment);
        }
    }
    result.reference_time = reference_time;
    const bool within_reference = result.time && reference_time &&
        within_time(*result.time, *reference_time, snapshot.max_reference_separation);
    if (within_reference && result.count && reference) {
        result.shift = belt_shift(snapshot.encoder, *result.count, *reference);
        if (result.shift) {
            result.aligned = shift_points(result.measured, *result.shift);
        }
    }
    if (result.measured.empty()) {
        result.issue = "no_spatial_projection";
    } else if (!result.time) {
        result.issue = "unverified_exposure_time";
    } else if (!result.count) {
        result.issue = "unverified_exposure_count";
    } else if (reference && !within_reference) {
        result.issue = "reference_outside_time_limit";
    } else if (reference && !result.shift) {
        result.issue = "reference_outside_displacement_limit";
    }
    return result;
}

}  // namespace

void CalibrationService::install(Snapshot snapshot) {
    snapshots_.install(std::move(snapshot));
}

void CalibrationService::retire(const std::string& run_id, std::uint64_t generation) {
    snapshots_.retire(run_id, generation);
}

Result CalibrationService::process(std::shared_ptr<const m01::Segment> segment,
                                    const ProcessContext& context) const {
    Result result;
    result.source = std::move(segment);
    result.acquisition_diagnostics.assign(context.acquisition_diagnostics.begin(),
                                          context.acquisition_diagnostics.end());
    if (!result.source || !result.source->config) {
        result.issue = "missing_acquisition_config";
        return result;
    }
    const auto& source = *result.source;
    const auto& config = *source.config;
    result.snapshot = snapshots_.find(source.run_id, source.generation);
    if (!result.snapshot || !matches(*result.snapshot, config)) {
        result.issue = "no_matching_calibration";
        return result;
    }
    const auto& snapshot = *result.snapshot;
    const auto samples = timeline(snapshot, source, context.sync_window);
    struct Reference {
        std::optional<CountEstimate> count;
        std::optional<TimeEstimate> time;
    };
    const auto resolve = [&](const m01::TriggerEvent* event) -> Reference {
        if (event && current(snapshot, config, *event)) {
            if (const auto time = sync_time(snapshot, *event)) {
                return {locked_count(snapshot.encoder, *event), time};
            }
        }
        return {};
    };
    const auto default_reference = resolve(context.independent_reference);
    if (context.independent_reference && !default_reference.count) {
        result.issue = "invalid_independent_reference";
    }
    std::map<m01::ObservationId, const m01::TriggerEvent*> references;
    for (const auto& binding : context.independent_references) {
        const auto [entry, inserted] = references.emplace(binding.observation, &binding.reference);
        if (!inserted) entry->second = nullptr;
    }

    for (const auto& item : source.items) {
        if (const auto* record = std::get_if<m01::TriggerRecord>(&item)) {
            AlignedTrigger group;
            group.source = *record;
            group.reference_count = locked_count(snapshot.encoder, record->event);
            const bool valid_reference = current(snapshot, config, record->event);
            if (valid_reference) {
                group.reference_time = sync_time(snapshot, record->event);
            }
            const auto reference = group.reference_time ? std::optional(group.reference_count) : std::nullopt;
            const auto common = config.routing == m01::Routing::SharedTrigger && valid_reference;
            const auto align_row = [&](const m01::Observation& row, Quality quality, m01::Source expected) {
                const bool associated = row.header.id.source == expected && row.header.association_verified &&
                                        row.header.trigger == record->event.key && common;
                if (quality == Quality::Valid && !associated) {
                    quality = Quality::Unknown;
                }
                return align(snapshot, config, row, quality, samples, associated ? record : nullptr,
                             associated ? reference : std::nullopt,
                             associated ? group.reference_time : std::nullopt, context.environment);
            };
            if (record->rgb) {
                group.rgb = align_row(*record->rgb, record->rgb_quality, m01::Source::Rgb);
            }
            if (record->hsi) {
                group.hsi = align_row(*record->hsi, record->hsi_quality, m01::Source::Hsi);
            }
            if (group.rgb && group.hsi) {
                if (group.rgb->source_quality != Quality::Valid || group.hsi->source_quality != Quality::Valid) {
                    group.coverage_status = CoverageStatus::UnusableInput;
                } else {
                    group.common_coverage = overlap(snapshot, *group.rgb, *group.hsi, &group.coverage_status);
                }
            }
            result.triggers.push_back(std::move(group));
        } else if (const auto* row = std::get_if<m01::Observation>(&item)) {
            const auto binding = references.find(row->header.id);
            const bool explicit_ref = binding != references.end();
            const auto reference = explicit_ref ? resolve(binding->second) : default_reference;
            auto observation = align(snapshot, config, *row, row->header.source_quality, samples,
                                     nullptr, reference.count, reference.time, context.environment);
            if (explicit_ref && !reference.count && observation.issue.empty()) {
                observation.issue = binding->second ? "invalid_independent_reference" : "ambiguous_independent_reference";
            }
            result.independent.push_back(std::move(observation));
        } else if (const auto* diagnostic = std::get_if<m01::Diagnostic>(&item)) {
            result.acquisition_diagnostics.push_back(*diagnostic);
        }
    }
    return result;
}

TransformResult<ProjectedPoint> CalibrationService::transform_point(const PointRequest& request) const {
    auto result = start_transform<ProjectedPoint>(snapshots_, request.reference);
    if (result.issue == TransformIssue::None) {
        result.value = m02::transform_point(*result.snapshot, request.from, request.to, request.point);
        if (result.value) {
            result.calibration_residual_mm = result.value->error_mm;
        } else {
            result.issue = TransformIssue::UnsupportedFrameOrOutOfDomain;
        }
    }
    return result;
}

TransformResult<RigidTransform> CalibrationService::transform_pose(const PoseRequest& request) const {
    auto result = start_transform<RigidTransform>(snapshots_, request.reference);
    if (result.issue != TransformIssue::None) {
        return result;
    }
    const auto& robot = result.snapshot->robot;
    if (!robot) {
        result.issue = TransformIssue::UnsupportedFrameOrOutOfDomain;
    } else if (robot->tool_id != request.tool_id) {
        result.issue = TransformIssue::ToolMismatch;
    } else {
        result.value = robot_pose(*result.snapshot, request.belt_from_tcp, request.tool_id, request.target);
        if (result.value) {
            result.calibration_residual_mm = robot->residual_mm;
        } else {
            result.issue = TransformIssue::UnsupportedFrameOrOutOfDomain;
        }
    }
    return result;
}

}  // namespace waste::m02
