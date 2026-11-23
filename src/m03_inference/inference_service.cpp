#include "inference_service.h"
#include "input_preprocessor.h"
#include "m02_calibration/calibration_snapshot.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace waste::m03 {
namespace {

Result status(WindowId window, const ModelIdentity& model, std::optional<m02::Versions> versions,
              State state, std::string reason, Tick now) {
    return {std::move(window), model, std::move(versions), state, std::move(reason), {}, now, now, now, {}, {}, {}, {}};
}

bool same_versions(const m02::Versions& a, const m02::Versions& b) {
    return a.geometry == b.geometry && a.clock == b.clock && a.encoder == b.encoder &&
           a.spectral_reference == b.spectral_reference;
}

bool current(const m01::Config& c, const m01::TriggerKey& key) {
    return key.sync_session == c.sync.session && key.encoder_epoch == c.encoder_epoch;
}

}  // namespace

InferenceService::InferenceService(std::size_t model_capacity, std::size_t active_capacity, QueueLimits queue,
                                   Clock clock)
    : registry_(model_capacity), active_capacity_(active_capacity), clock_(std::move(clock)),
      queue_(queue, clock_) {
    if (!active_capacity) {
        throw std::invalid_argument("active generation capacity must be positive");
    }
}

void InferenceService::install(Manifest manifest, std::span<const std::byte> artifact, Backend& backend) {
    registry_.install(std::move(manifest), artifact, backend);
}

bool InferenceService::unload(const ModelKey& key) {
    return registry_.unload(key);
}

void InferenceService::activate(const m01::Config& acquisition, const m02::Snapshot& calibration,
                                const Activation& activation) {
    const Generation generation{acquisition.run_id, acquisition.generation};
    const auto previous = last_generation_.find(generation.run_id);
    if (active_.size() >= active_capacity_ ||
        (previous != last_generation_.end() && generation.config_epoch <= previous->second) ||
        activation.models.empty() || activation.models.size() > 2 ||
        (acquisition.first_expected_trigger && activation.first_trigger != *acquisition.first_expected_trigger) ||
        !m02::matches(calibration, acquisition)) {
        throw std::invalid_argument("invalid activation generation, model set, or acquisition identity");
    }
    Active entry{acquisition, calibration.versions, {}};
    std::set<Route> routes;
    for (const auto& key : activation.models) {
        auto model = registry_.find(key);
        if (!model || !routes.insert(model->manifest.route).second ||
            (activation.models.size() > 1 && model->manifest.route == Route::Joint)) {
            throw std::invalid_argument("model is unavailable or activation routes overlap");
        }
        verify_compatibility(model->manifest, acquisition, calibration);
        if (model->manifest.window.rows - 1 > std::numeric_limits<std::uint64_t>::max() - activation.first_trigger ||
            input_bytes(model->manifest) > queue_.byte_limit()) {
            throw std::invalid_argument("window trigger or tensor exceeds configured limits");
        }
        entry.branches.push_back({model, WindowBuilder(generation, model->manifest, activation.first_trigger)});
    }
    active_.emplace(generation, std::move(entry));
    last_generation_[generation.run_id] = generation.config_epoch;
}

void InferenceService::check_time(Tick now) {
    if (now < input_time_) {
        throw std::invalid_argument("inference clock must be monotonic and nonnegative");
    }
    input_time_ = now;
    now_ = current_tick(clock_, now);
}

void InferenceService::build(Active& active, Report& report) {
    for (auto& branch : active.branches) {
        build_branch(branch, active.versions, report);
    }
}

void InferenceService::build_branch(Branch& branch, const m02::Versions& versions, Report& report) {
    while (auto window = branch.builder.take(current_tick(clock_, now_))) {
        auto result = status(window->id, branch.model->manifest.identity, versions,
                             State::InputInvalid, "", window->ready);
        result.first_received = window->first_received;
        result.ready = window->ready;
        result.deadline = window->valid_until;
        if (window->ready >= result.deadline) {
            result.state = State::Expired;
            result.reason = "expired_waiting_for_window";
            report.windows.push_back(std::move(result));
            continue;
        }
        auto preparation = prepare(branch.model->manifest, *window);
        result.input_quality = std::move(preparation.quality);
        const auto prepared = current_tick(clock_, window->ready);
        result.prepared = prepared;
        if (!preparation.input) {
            result.reason = std::move(preparation.issue);
            result.finished = prepared;
            report.windows.push_back(std::move(result));
        } else {
            queue_.enqueue({std::move(result), branch.model, std::move(preparation.input)}, prepared, report);
        }
    }
}

Report InferenceService::ingest(std::shared_ptr<const m02::Result> input, Tick now, Tick valid_until) {
    check_time(now);
    Report report;
    queue_.advance(now_, report);
    if (input) {
        report.acquisition_diagnostics = input->acquisition_diagnostics;
    }
    const auto source = input ? input->source : nullptr;
    const Generation generation{source ? source->run_id : "", source ? source->generation : 0};
    const auto found = active_.find(generation);
    if (!source || !source->config || !input->snapshot || !input->issue.empty() ||
        found == active_.end() || !m02::matches(*input->snapshot, *source->config) ||
        !m02::matches(*input->snapshot, found->second.acquisition) ||
        !same_versions(input->snapshot->versions, found->second.versions) || valid_until.count() < 0) {
        report.windows.push_back(status({generation, {}, 0, 0}, {}, {}, State::Rejected,
                                        "observation_or_generation_mismatch", now_));
        return deliver(std::move(report));
    }
    auto& active = found->second;
    build(active, report);
    for (auto& branch : active.branches) {
        const auto& identity = branch.model->manifest.identity;
        const auto note = [&](Insert state, std::uint64_t first, std::uint64_t last) {
            if (state == Insert::OutOfRange) {
                report.windows.push_back(status({generation, identity.key, first, last}, identity,
                                                active.versions, State::Rejected,
                                                "trigger_outside_window_lookahead", now_));
            }
        };
        for (std::size_t i = 0; i < input->triggers.size(); ++i) {
            const auto& key = input->triggers[i].source.event.key;
            if (!current(active.acquisition, key)) {
                report.windows.push_back(status({generation, identity.key, key.id, key.id}, identity,
                                                active.versions, State::Rejected, "source_epoch_mismatch", now_));
                continue;
            }
            auto inserted = branch.builder.add(key.id, {input, i, now_, valid_until});
            if (inserted == Insert::OutOfRange) {
                build_branch(branch, active.versions, report);
                inserted = branch.builder.add(key.id, {input, i, now_, valid_until});
            }
            note(inserted, key.id, key.id);
        }
        for (const auto& gap : input->acquisition_diagnostics) {
            if (gap.issue == m01::Issue::MissingSync && gap.source == m01::Source::Sync &&
                gap.first_trigger && gap.last_trigger && current(active.acquisition, *gap.first_trigger) &&
                current(active.acquisition, *gap.last_trigger) &&
                (!gap.input_generation || *gap.input_generation == generation.config_epoch)) {
                note(branch.builder.mark_gap(gap.first_trigger->id, gap.last_trigger->id),
                     gap.first_trigger->id, gap.last_trigger->id);
            }
        }
    }
    build(active, report);
    queue_.advance(now_, report);
    return deliver(std::move(report));
}

Report InferenceService::advance(Tick now) {
    check_time(now);
    Report report;
    queue_.advance(now_, report);
    for (auto& [generation, active] : active_) {
        build(active, report);
    }
    queue_.advance(now_, report);
    return deliver(std::move(report));
}

Report InferenceService::seal_through(const Generation& generation, std::uint64_t trigger, Tick now) {
    check_time(now);
    Report report;
    queue_.advance(now_, report);
    const auto found = active_.find(generation);
    if (found == active_.end()) {
        throw std::invalid_argument("cannot seal unknown generation");
    }
    for (auto& branch : found->second.branches) {
        const auto& identity = branch.model->manifest.identity;
        if (branch.builder.seal_through(trigger) == Insert::OutOfRange) {
            report.windows.push_back(status({generation, identity.key, trigger, trigger}, identity,
                                            found->second.versions, State::Rejected,
                                            "seal_outside_window_lookahead", now_));
        }
    }
    build(found->second, report);
    queue_.advance(now_, report);
    return deliver(std::move(report));
}

Report InferenceService::retire(const Generation& generation, Tick now, bool drain_approved_results) {
    check_time(now);
    Report report;
    const auto found = active_.find(generation);
    if (found != active_.end()) {
        for (const auto& branch : found->second.branches) {
            if (const auto window = branch.builder.unfinished()) {
                report.windows.push_back(status(*window, branch.model->manifest.identity, found->second.versions,
                                                State::Retired, "unfinished_window_at_boundary", now_));
            }
        }
        active_.erase(found);
    }
    if (!drain_approved_results) {
        queue_.discard(generation, now_, report);
    }
    queue_.advance(now_, report);
    return deliver(std::move(report));
}

Report InferenceService::deliver(Report report) const {
    const auto delivery_time = current_tick(clock_, now_);
    for (auto& result : report.windows) {
        if ((result.state == State::Completed || result.state == State::NoDetection) &&
            delivery_time >= result.deadline) {
            result.state = State::Expired;
            result.reason = "expired_at_report_delivery";
            result.evidence.clear();
        }
    }
    return report;
}

DeploymentStatus InferenceService::deployment_status() const {
    DeploymentStatus result;
    result.loaded = registry_.loaded();
    for (const auto& [generation, entry] : active_) {
        auto& models = result.active[generation];
        for (const auto& branch : entry.branches) {
            models.push_back(branch.model->manifest.identity);
        }
    }
    return result;
}

QueueHealth InferenceService::health() const {
    return queue_.health(now_);
}

}  // namespace waste::m03
