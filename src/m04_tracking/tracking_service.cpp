#include "tracking_service.h"
#include "material_fusion.h"
#include "surface_fusion.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace waste::m04 {
namespace {

Point from_belt(m02::Point p) { return {p.x, p.y, p.z}; }

std::optional<Reference> reference(const std::optional<m02::TimeEstimate>& time,
                                    const std::optional<m02::CountEstimate>& count) {
    if (!time || !count) {
        return {};
    }
    return Reference{time->value, time->error, count->epoch, count->value, count->error_counts};
}

bool usable_point(const TrackingConfig& c, const m02::ProjectedPoint& point,
                  const std::optional<Reference>& ref, Tick now, std::optional<Tick> barrier, Tick age) {
    return point.quality == m01::Quality::Valid && std::isfinite(point.position.x) &&
           std::isfinite(point.position.y) && std::isfinite(point.position.z) &&
           std::isfinite(point.error_mm) && point.error_mm >= 0 && ref && valid_reference(c, *ref) &&
           ref->time <= now && (!barrier || ref->time - ref->time_error > *barrier) &&
           now < reference_expiry(*ref, age);
}

std::optional<Sample> from_m03(const TrackingConfig& c, const m03::SampleLocation& input,
                                Tick now, std::optional<Tick> barrier, Tick age) {
    const auto ref = reference(input.time, input.count);
    if (!usable_point(c, input.belt, ref, now, barrier, age) ||
        input.source.trigger.encoder_epoch != c.encoder.epoch) {
        return {};
    }
    if (input.applied_shift &&
        (input.applied_shift->to.epoch != ref->encoder_epoch ||
         input.applied_shift->to.value != ref->count ||
         input.applied_shift->to.error_counts != ref->count_error)) {
        return {};
    }
    const auto& raw = input.source;
    return Sample{{{raw.observation, raw.column}, raw.segment_sequence, raw.source_bytes},
                  *ref, from_belt(input.belt.position), input.belt.error_mm, *ref,
                  input.applied_shift ? std::optional(input.applied_shift->mm) : std::nullopt};
}

bool valid_window_size(const m03::Result& window, const Limits& limits) {
    if (window.evidence.size() > limits.max_window_instances) {
        return false;
    }
    std::size_t count = 0;
    for (const auto& item : window.evidence) {
        if (item.samples.size() > limits.max_window_samples - count) {
            return false;
        }
        count += item.samples.size();
    }
    return true;
}

bool trusted_score(const m03::Evidence& evidence, const ModelPolicy& policy) {
    return evidence.score_kind == policy.material_score_kind && std::isfinite(evidence.score) &&
           evidence.score >= policy.min_material_score &&
           (evidence.score_kind != m03::ScoreKind::Probability || evidence.score <= 1);
}

struct Owned {
    std::vector<const SpectralPoint*> material;
    std::vector<SampleKey> ambiguous;
    std::vector<const Sample*> surface;
};

}  // namespace

void TrackingService::check_time(Tick now) {
    if (now < now_) {
        throw std::invalid_argument("tracking clock must be monotonic and nonnegative");
    }
    now_ = now;
}

Report TrackingService::activate(TrackingConfig config, Tick now) {
    validate(config);
    const auto previous = highest_generation_.find(config.generation.run_id);
    if (previous != highest_generation_.end() && config.generation.config_epoch <= previous->second) {
        throw std::invalid_argument("tracking generation must increase");
    }
    check_time(now);
    Report report;
    for (const auto* target : targets_.active()) {
        targets_.invalidate(target->id, Reason::ConfigurationRetired);
    }
    targets_.publish(report);
    highest_generation_[config.generation.run_id] = config.generation.config_epoch;
    config_ = std::move(config);
    evidence_.emplace(config_->limits);
    seen_windows_.clear();
    encoder_.reset();
    last_encoder_time_.reset();
    motion_fault_.reset();
    motion_barrier_.reset();
    birth_barrier_.reset();
    retired_.clear();
    return report;
}

void TrackingService::fail_motion(Reason reason) {
    for (const auto* target : targets_.active()) {
        retire(target->id, reason);
    }
    motion_fault_ = reason;
    motion_barrier_ = now_;
    encoder_.reset();
    evidence_->discard_before(now_);
}

void TrackingService::retire(TargetId id, Reason reason) {
    const auto* target = targets_.find(id);
    if (!target || target->state == TargetState::Invalid) {
        return;
    }
    if (retired_.size() >= config_->limits.max_targets) {
        const auto oldest = std::ranges::min_element(retired_, {}, [](const auto& entry) {
            return entry.second.retired_at;
        });
        const auto cutoff = oldest->second.retired_at;
        birth_barrier_ = std::max(birth_barrier_.value_or(cutoff), cutoff);
        evidence_->discard_before(cutoff);
        retired_.erase(oldest);
    }
    Target contour;
    contour.id = id;
    contour.state = TargetState::Invalid;
    contour.footprint = target->footprint;
    retired_.emplace(id, Retired{std::move(contour), now_});
    targets_.invalidate(id, reason);
}

std::vector<const Target*> TrackingService::retired_for(Tick observation_time) const {
    std::vector<const Target*> result;
    for (const auto& [id, entry] : retired_) {
        if (observation_time <= entry.retired_at) {
            result.push_back(&entry.contour);
        }
    }
    return result;
}

void TrackingService::check_targets() {
    for (const auto* target : targets_.active()) {
        if (now_ >= target->valid_until) {
            retire(target->id, Reason::TargetExpired);
        } else if (encoder_ && target->state == TargetState::Tracked) {
            const auto prediction = predict_position(*config_, *target, &*encoder_, now_);
            if (prediction.reason == Reason::OutsideRange || prediction.reason == Reason::MotionUnverified) {
                retire(target->id, prediction.reason);
            }
        }
    }
}

void TrackingService::step(Tick now, Report& report) {
    check_time(now);
    if (!config_) {
        return;
    }
    if (encoder_) {
        const auto issue = encoder_issue(*config_, *encoder_, now_);
        if (issue != Reason::None) {
            fail_motion(issue);
        }
    }
    check_targets();
    evidence_->expire(now_, config_->max_evidence_age);
    const auto horizon = std::max(config_->max_evidence_age, config_->max_target_age);
    std::erase_if(retired_, [&](const auto& entry) {
        return now_ >= expires_at(entry.second.retired_at, horizon);
    });
    std::erase_if(seen_windows_, [&](const auto& entry) { return now_ >= entry.second; });
    // Free invalid targets before admitting new input in this operation.
    targets_.publish(report);
}

void TrackingService::apply_instances(std::span<const Instance> instances, Report& report) {
    const auto active = targets_.active();
    auto decisions = associate(*config_, instances, active);
    for (std::size_t i = 0; i < instances.size(); ++i) {
        auto& decision = decisions[i];
        const auto& footprint = instances[i].footprint;
        auto expiry = reference_expiry(footprint.reference, config_->max_target_age);
        if (decision.reason == Reason::AssociationAmbiguous) {
            for (auto id : decision.candidates) {
                auto& target = *targets_.find(id);
                if (footprint.reference.time >= target.association_time &&
                    target.state != TargetState::Ambiguous) {
                    target.state = TargetState::Ambiguous;
                    target.reason = Reason::AssociationAmbiguous;
                    target.association_time = footprint.reference.time;
                    targets_.changed(id);
                } else if (target.state == TargetState::Ambiguous &&
                           footprint.reference.time > target.association_time) {
                    target.association_time = footprint.reference.time;
                    targets_.changed(id);
                }
            }
        } else if (decision.matched) {
            auto& target = *targets_.find(*decision.matched);
            if (footprint.reference.time > target.footprint.reference.time) {
                target.footprint = footprint;
                target.valid_until = expiry;
                targets_.changed(target.id);
            }
            // An older observation can add evidence, but cannot clear a newer doubt.
            if (target.state == TargetState::Ambiguous &&
                footprint.reference.time > target.association_time) {
                target.state = TargetState::Tracked;
                target.reason = Reason::None;
                target.association_time = footprint.reference.time;
                targets_.changed(target.id);
            }
            if (target.state == TargetState::Tracked) {
                target.association_time = std::max(target.association_time, footprint.reference.time);
            }
        } else {
            const auto retired = retired_for(footprint.reference.time);
            if (birth_barrier_ && footprint.reference.time <= *birth_barrier_) {
                decision.reason = Reason::InputExpired;
            } else if (!retired.empty()) {
                const auto previous = associate(*config_, instances.subspan(i, 1), retired);
                if (!previous.front().candidates.empty()) {
                    decision.reason = Reason::AssociationAmbiguous;
                    decision.candidates = previous.front().candidates;
                }
            }
            if (decision.reason != Reason::None) {
                report.associations.push_back(std::move(decision));
                continue;
            }
            decision.matched = targets_.insert(config_->generation, footprint, expiry,
                                                 config_->limits.max_targets);
            if (!decision.matched) {
                decision.reason = Reason::Capacity;
            }
        }
        report.associations.push_back(std::move(decision));
    }
}

void TrackingService::refresh_evidence() {
    const auto active = targets_.active();
    if (active.empty()) {
        return;
    }
    const auto locate = [&](const Sample& sample) {
        auto retired = retired_for(sample.reference.time);
        if (retired.empty()) {
            return locate_owner(*config_, sample, active);
        }
        retired.insert(retired.begin(), active.begin(), active.end());
        return locate_owner(*config_, sample, retired);
    };
    std::map<TargetId, Owned> owned;
    for (const auto& [key, point] : evidence_->material()) {
        const auto ownership = locate(point.sample);
        if (ownership.target) {
            owned[*ownership.target].material.push_back(&point);
        } else {
            for (auto id : ownership.plausible) {
                owned[id].ambiguous.push_back(key);
            }
        }
    }
    for (const auto& [key, point] : evidence_->surfaces()) {
        if (auto owner = locate(point).target) {
            owned[*owner].surface.push_back(&point);
        }
    }
    for (const auto* current : active) {
        auto& target = *targets_.find(current->id);
        auto& samples = owned[target.id];
        auto material = fuse_material(*config_, samples.material, std::move(samples.ambiguous));
        auto surface = fuse_surface(*config_, target.footprint.reference, samples.surface);
        if (target.material != material || target.surfaces != surface) {
            target.material = std::move(material);
            target.surfaces = std::move(surface);
            targets_.changed(target.id);
        }
    }
}

void TrackingService::close(Report& report) {
    if (config_) {
        check_targets();
        refresh_evidence();
        targets_.publish(report);
    }
}

Report TrackingService::ingest_window(const m03::Result& window, Tick now) {
    Report report;
    step(now, report);
    const WindowKey key{window.window.model, window.window.first, window.window.last};
    const auto diagnostic = [&](Reason reason) {
        report.evidence.push_back({Origin{key, 0}, {}, reason});
    };
    Reason failure = Reason::None;
    const auto* policy = config_ ? model_policy(*config_, window.model) : nullptr;
    if (!config_) {
        failure = Reason::Unconfigured;
    } else if (window.window.generation != config_->generation) {
        failure = Reason::WrongGeneration;
    } else if (motion_fault_) {
        failure = *motion_fault_;
    } else if (window.state == m03::State::Expired) {
        failure = Reason::InputExpired;
    } else if (!policy || key.model != window.model.key || key.first > key.last ||
               !window.calibration || !same_calibration(*window.calibration, config_->calibration) ||
               (window.state != m03::State::Completed && window.state != m03::State::NoDetection) ||
               (window.state == m03::State::NoDetection && !window.evidence.empty())) {
        failure = Reason::InvalidInput;
    } else if (now_ >= window.deadline) {
        failure = Reason::InputExpired;
    } else if (seen_windows_.contains(key)) {
        failure = Reason::Duplicate;
    } else if (seen_windows_.size() >= config_->limits.max_recent_windows ||
               !valid_window_size(window, config_->limits)) {
        failure = Reason::Capacity;
    }
    if (failure != Reason::None) {
        diagnostic(failure);
        close(report);
        return report;
    }

    seen_windows_.emplace(key, window.deadline);
    std::vector<Instance> instances;
    std::set<std::size_t> seen_instances;
    for (const auto& item : window.evidence) {
        const Origin origin{key, item.instance};
        if (!seen_instances.insert(item.instance).second) {
            report.evidence.push_back({origin, {}, Reason::InvalidInput});
            continue;
        }
        const auto label = policy->materials.find(item.label);
        const bool has_material = (item.kind == m03::EvidenceKind::Material ||
                                   item.kind == m03::EvidenceKind::JointInstance) &&
                                  label != policy->materials.end() && trusted_score(item, *policy);
        const bool has_contour = item.kind == m03::EvidenceKind::Contour ||
                                 item.kind == m03::EvidenceKind::JointInstance;
        std::vector<Sample> contour;
        bool contour_full = false;
        std::set<SampleKey> unique;
        for (const auto& raw : item.samples) {
            const auto row = raw.source.trigger.id;
            if (row < key.first || row > key.last) {
                continue;
            }
            const auto source = raw.source.observation.source;
            if (has_contour && source == m01::Source::Rgb) {
                auto sample = from_m03(*config_, raw, now_, motion_barrier_, config_->max_target_age);
                if (sample && unique.insert(sample->source.key).second) {
                    if (contour.size() >= config_->limits.max_instance_points) {
                        contour_full = true;
                    } else {
                        contour.push_back(std::move(*sample));
                    }
                }
            }
            if (has_material && source == m01::Source::Hsi) {
                if (auto sample = from_m03(*config_, raw, now_, motion_barrier_, config_->max_evidence_age)) {
                    const auto source_ref = sample->source;
                    const auto issue = evidence_->add_material(std::move(*sample),
                                                                {origin, label->second, item.score});
                    report.evidence.push_back({origin, source_ref, issue});
                }
            }
        }
        if (has_contour) {
            if (!contour_full) {
                if (auto footprint = make_footprint(*config_, std::move(contour))) {
                    instances.push_back({origin, std::move(*footprint)});
                    continue;
                }
            }
            report.associations.push_back({origin, {}, {},
                                           contour_full ? Reason::Capacity : Reason::InsufficientContour});
        }
    }
    apply_instances(instances, report);
    close(report);
    return report;
}

Report TrackingService::ingest_surfaces(const m02::Result& observations, Tick now) {
    Report report;
    step(now, report);
    if (!config_ || !observations.source || !observations.snapshot ||
        !observations.issue.empty() ||
        observations.source->run_id != config_->generation.run_id ||
        observations.source->generation != config_->generation.config_epoch ||
        !same_calibration(observations.snapshot->versions, config_->calibration) || motion_fault_) {
        report.evidence.push_back({{}, {}, motion_fault_.value_or(config_ ? Reason::InvalidInput
                                                                          : Reason::Unconfigured)});
        close(report);
        return report;
    }
    std::size_t budget = config_->limits.max_window_samples;
    for (const auto& row : observations.independent) {
        const auto& raw = row.source.header;
        if (raw.id.source != m01::Source::E05) {
            continue;
        }
        const bool aligned = row.shift && row.reference_time && !row.aligned.empty();
        const auto& points = aligned ? row.aligned : row.measured;
        const auto ref = reference(aligned ? row.reference_time : row.time,
                                   aligned ? std::optional(row.shift->to) : row.count);
        if (points.size() > budget) {
            report.evidence.push_back({{}, SourceRef{{raw.id, 0}, observations.source->sequence, raw.bytes},
                                       Reason::Capacity});
            break;
        }
        budget -= points.size();
        if (points.empty()) {
            report.evidence.push_back({{}, SourceRef{{raw.id, 0}, observations.source->sequence, raw.bytes},
                                       Reason::SurfaceMissing});
        }
        for (std::size_t i = 0; i < points.size(); ++i) {
            const SourceRef ref_id{{raw.id, i}, observations.source->sequence, raw.bytes};
            const auto& point = points[i];
            auto issue = Reason::InvalidInput;
            if (point.quality == m01::Quality::Missing || row.source_quality == m01::Quality::Missing) {
                issue = Reason::SurfaceMissing;
            } else if (row.source_quality == m01::Quality::Valid &&
                       usable_point(*config_, point, ref, now_, motion_barrier_, config_->max_evidence_age)) {
                if (point.position.z - point.error_mm < config_->min_surface_height_mm) {
                    // Valid observation of a surface below the verified material region.
                    issue = Reason::SurfaceUnassigned;
                } else {
                    issue = evidence_->add_surface(
                        {ref_id, *ref, from_belt(point.position), point.error_mm, *ref,
                         aligned ? std::optional(row.shift->mm) : std::nullopt});
                }
            }
            report.evidence.push_back({{}, ref_id, issue});
        }
    }
    close(report);
    return report;
}

Report TrackingService::update_encoder(EncoderState state, Tick now) {
    Report report;
    check_time(now);
    if (!config_ || state.generation != config_->generation) {
        report.evidence.push_back({{}, {}, config_ ? Reason::WrongGeneration : Reason::Unconfigured});
    } else if (motion_fault_ == Reason::EncoderReset) {
        report.evidence.push_back({{}, {}, Reason::EncoderReset});
    } else if (last_encoder_time_ && state.reference.time < *last_encoder_time_) {
        report.evidence.push_back({{}, {}, Reason::EncoderStale});
    } else if (encoder_ && state == *encoder_) {
        report.evidence.push_back({{}, {}, Reason::Duplicate});
    } else {
        auto issue = encoder_issue(*config_, state, now_);
        if (issue == Reason::None && encoder_) {
            const auto& a = encoder_->reference;
            const auto& b = state.reference;
            const auto delta = (b.count - a.count) * config_->encoder.signed_mm_per_count;
            const auto error = (a.count_error + b.count_error) * std::abs(config_->encoder.signed_mm_per_count);
            if (a.time == b.time || !std::isfinite(delta) ||
                delta * config_->belt_direction < -error ||
                (state.direction == 0 && encoder_->direction == 0 && std::abs(delta) > error)) {
                issue = Reason::MotionUnverified;
            }
        }
        if (issue == Reason::None) {
            if (encoder_ && now_ >= reference_expiry(encoder_->reference, config_->max_encoder_age)) {
                fail_motion(Reason::EncoderStale);
                report.evidence.push_back({{}, {}, Reason::EncoderStale});
            }
            encoder_ = std::move(state);
            last_encoder_time_ = encoder_->reference.time;
            motion_fault_.reset();
        } else {
            if (state.reference.time >= Tick::zero() && state.reference.time <= now_) {
                last_encoder_time_ = state.reference.time;
            }
            fail_motion(issue);
            report.evidence.push_back({{}, {}, issue});
        }
    }
    step(now, report);
    close(report);
    return report;
}

Report TrackingService::advance(Tick now) {
    Report report;
    step(now, report);
    close(report);
    return report;
}

Report TrackingService::invalidate_motion(TargetId id, std::uint64_t expected_version, Tick now) {
    Report report;
    step(now, report);
    if (const auto* target = targets_.find(id); target && target->version == expected_version) {
        retire(id, Reason::MotionUnverified);
    } else {
        report.evidence.push_back({{}, {}, target ? Reason::VersionMismatch : Reason::TargetMissing});
    }
    close(report);
    return report;
}

std::optional<Target> TrackingService::target(TargetId id) const {
    if (const auto* value = targets_.find(id)) {
        return *value;
    }
    return {};
}

Prediction TrackingService::predict(TargetId id, std::uint64_t expected_version, Tick now) const {
    Prediction result;
    result.target = id;
    result.version = expected_version;
    if (!config_) {
        result.reason = Reason::Unconfigured;
    } else if (now < now_) {
        result.reason = Reason::InvalidInput;
    } else if (const auto* target = targets_.find(id); !target) {
        result.reason = Reason::TargetMissing;
    } else if (target->version != expected_version) {
        result.reason = Reason::VersionMismatch;
        result.version = target->version;
        result.generation = target->generation;
    } else {
        return predict_position(*config_, *target, encoder_ ? &*encoder_ : nullptr, now);
    }
    return result;
}

}  // namespace waste::m04
