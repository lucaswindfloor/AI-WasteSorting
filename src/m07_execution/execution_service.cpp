#include "execution_service.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace waste::m07 {
namespace {

const FieldState* field_of(const DurableState& s) { return s.field ? &*s.field : nullptr; }

bool command_same_as_selected(const m05::Candidate& current, const m05::Candidate& original) {
    return current.target == original.target && current.recipe_version == original.recipe_version &&
           current.profile_version == original.profile_version && current.proof_id == original.proof_id &&
           current.region_id == original.region_id && current.bin_mapping_version == original.bin_mapping_version &&
           current.rule_label == original.rule_label && current.tool.identity == original.tool.identity &&
           current.bin.identity == original.bin.identity &&
           current.bin.placement_revision == original.bin.placement_revision &&
           current.contact.center == original.contact.center && current.contact.sources == original.contact.sources;
}

bool overlaps(m06::Interval a, m06::Interval b) {
    return a.begin < b.end && b.begin < a.end;
}

}  // namespace

ExecutionService::ExecutionService(ExecutionConfig config, ControllerPort& controller, IntentStorePort& store,
                                   const m05::EligibilityService& eligibility, m06::TargetSource& targets,
                                   Clock clock)
    : config_(std::move(config)), controller_(controller), store_(store),
      eligibility_(eligibility), targets_(targets), clock_(std::move(clock)) {
    validate(config_);
    if (!clock_) {
        throw std::invalid_argument("execution requires a monotonic clock");
    }
    state_.generation = config_.generation;
    state_.configuration = config_.version;
}

Tick ExecutionService::time() {
    const auto sampled = clock_();
    if (sampled < now_) {
        ready_ = false;
        armed_here_.clear();
        throw std::runtime_error("execution clock went backwards");
    }
    now_ = sampled;
    return now_;
}

bool ExecutionService::commit(DurableState next) {
    if (state_.revision == std::numeric_limits<std::uint64_t>::max() ||
        transaction_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        ready_ = false;
        armed_here_.clear();
        return false;
    }
    next.revision = state_.revision + 1;
    next.transaction = config_.writer_session + "/" + std::to_string(++transaction_sequence_);
    next.updated = time();
    try {
        const auto confirmation = store_.commit(state_.revision, next);
        if (confirmation.status == StoreStatus::Confirmed && confirmation.revision == next.revision &&
            confirmation.transaction == next.transaction) {
            state_ = std::move(next);
            return true;
        }
    } catch (...) {
    }
    // The authoritative head might have changed. Reload before making any
    // more decisions, and never transmit an intent armed by an earlier head.
    ready_ = false;
    armed_here_.clear();
    return false;
}

bool ExecutionService::recover() {
    ready_ = false;
    armed_here_.clear();
    const auto now = time();
    LoadReply loaded;
    try {
        loaded = store_.load();
    } catch (...) {
        return false;
    }
    if (loaded.status == LoadStatus::Restored && loaded.state &&
        compatible_checkpoint(*loaded.state, config_, now) &&
        loaded.state->revision >= state_.revision) {
        state_ = std::move(*loaded.state);
    } else if (loaded.status == LoadStatus::Empty && !loaded.state && !state_.revision) {
        state_ = {};
        state_.generation = config_.generation;
        state_.configuration = config_.version;
    } else {
        return false;
    }
    auto recovered = state_;
    mark_restarted(recovered);
    ready_ = commit(std::move(recovered));
    return ready_;
}

Report ExecutionService::report(std::optional<m06::AttemptId> attempt, Issue issue,
                                std::optional<RequestId> request) const {
    return {std::move(attempt), std::move(request), issue, state_.revision};
}

std::optional<DurableState> ExecutionService::state() const {
    return ready_ ? std::optional(state_) : std::nullopt;
}

std::optional<m06::ExecutionSnapshot> ExecutionService::snapshot() {
    const auto now = time();
    return ready_ && state_.field ? std::optional(build_snapshot(state_, config_, now)) : std::nullopt;
}

Report ExecutionService::update_field(FieldState field) {
    const auto now = time();
    if (!ready_) {
        return report({}, Issue::NotReady);
    }
    if (field.sequence <= state_.field_sequence ||
        (state_.field && field.observed < state_.field->observed) ||
        !valid_field(config_, field, now)) {
        return report({}, Issue::InvalidInput);
    }
    auto next = state_;
    next.field_sequence = field.sequence;
    next.field = std::move(field);
    for (auto& [id, attempt] : next.attempts) {
        if (!closed(attempt.stage) && has_live_effect(attempt) && attempt.controller) {
            const auto* link = current_link(next.field->controllers, attempt.controller->device,
                                            now, config_.max_field_age);
            if (!link || link->session != attempt.controller->session) {
                attempt.needs_query = true;
                if (attempt.issue != Issue::EvidenceConflict) {
                    attempt.issue = Issue::QueryUnresolved;
                }
            }
        }
        refresh_stage(attempt, config_, field_of(next), now);
    }
    return report({}, commit(std::move(next)) ? Issue::None : Issue::PersistenceUnavailable);
}

m06::ReserveReply ExecutionService::reserve(const m06::ReservationRequest& request) {
    const auto now = time();
    if (!ready_) {
        return {m06::Reply::Uncertain, {}};
    }
    if (const auto it = state_.attempts.find(request.attempt); it != state_.attempts.end()) {
        return same_reservation_request(it->second.original, request)
               ? m06::ReserveReply{m06::Reply::Accepted, it->second.reservation}
               : m06::ReserveReply{m06::Reply::Rejected, {}};
    }
    const auto issue = check_reservation(state_, config_, request, now);
    if (issue != Issue::None || state_.attempts.size() >= config_.max_attempts) {
        return {issue == Issue::StaleRevision || issue == Issue::ResourceConflict
                ? m06::Reply::Conflict : m06::Reply::Rejected, {}};
    }
    m06::Reservation r{request.attempt, config_.writer_session + "/hold/" + std::to_string(state_.revision + 1),
                       request.uses, std::min(request.handoff_deadline + Tick(1), config_.valid_until)};
    auto next = state_;
    AttemptRecord record;
    record.original = request;
    record.reservation = r;
    next.attempts.emplace(request.attempt, std::move(record));
    return commit(std::move(next)) ? m06::ReserveReply{m06::Reply::Accepted, std::move(r)}
                                  : m06::ReserveReply{m06::Reply::Uncertain, {}};
}

Issue ExecutionService::live_task(const AttemptRecord& attempt, Tick now) {
    if (!attempt.task) {
        return Issue::BadIdentity;
    }
    const auto& task = *attempt.task;
    if (const auto issue = check_task(config_, state_, attempt, task, now); issue != Issue::None) {
        return issue;
    }
    const auto& field = *state_.field;
    for (const auto& [id, other] : state_.attempts) {
        if (id != attempt.original.attempt && !closed(other.stage) && has_live_effect(other) &&
            std::ranges::any_of(other.reservation.uses, [&](const auto& busy) {
                return std::ranges::any_of(attempt.reservation.uses, [&](const auto& mine) {
                    return mine.resource == busy.resource;
                });
            })) {
            return Issue::ResourceConflict;
        }
    }
    for (const auto& use : attempt.reservation.uses) {
        const auto calendar = std::ranges::find(field.calendars, use.resource, &m06::ResourceCalendar::resource);
        const m06::Interval rest{std::max(now, use.time.begin), use.time.end};
        if (!m06::valid(rest) || calendar == field.calendars.end() || now >= calendar->valid_until ||
            !std::ranges::any_of(calendar->usable, [&](auto i) { return i.begin <= rest.begin && i.end >= rest.end; }) ||
            std::ranges::any_of(calendar->occupied, [&](auto i) { return overlaps(i, rest); })) {
            return Issue::ResourceConflict;
        }
    }
    try {
        const auto current = targets_.read(attempt.original.attempt.target, now);
        if (!current || m05::binding(current->target) != task.candidate.target ||
            current->prediction.reason != m04::Reason::None ||
            current->prediction.target != task.candidate.target.id ||
            current->prediction.version != task.candidate.target.version ||
            current->prediction.generation != task.candidate.target.generation ||
            current->prediction.origin != task.candidate.target.reference ||
            current->prediction.current.time > now || now >= current->prediction.fresh_until ||
            current->prediction.current.encoder_epoch != task.reference.encoder_epoch ||
            current->forecast.generation != task.candidate.target.generation ||
            current->forecast.reference != current->prediction.current ||
            current->forecast.version != task.forecast_version ||
            !std::isfinite(current->forecast.min_mm_per_second) ||
            !std::isfinite(current->forecast.max_mm_per_second) ||
            current->forecast.min_mm_per_second <= 0 ||
            current->forecast.max_mm_per_second < current->forecast.min_mm_per_second ||
            task.contact.instants.close >= current->prediction.valid_until ||
            task.contact.instants.close >= current->forecast.valid_until) {
            return Issue::TargetChanged;
        }
        auto snapshot = field.eligibility;
        snapshot.revision = state_.revision;
        const auto checked = eligibility_.evaluate(current->target, snapshot,
                                                   {current->grids, current->regions}, now);
        const auto fresh = std::ranges::find_if(checked.candidates, [&](const auto& candidate) {
            return command_same_as_selected(candidate, task.candidate) &&
                   task.contact.instants.close < candidate.valid_until;
        });
        return fresh != checked.candidates.end() ? Issue::None : Issue::TargetChanged;
    } catch (...) {
        return Issue::TargetChanged;
    }
}

m06::Reply ExecutionService::handoff(const m06::Task& task) {
    const auto now = time();
    if (!ready_) {
        return m06::Reply::Uncertain;
    }
    const auto it = state_.attempts.find(task.attempt);
    if (it == state_.attempts.end()) {
        return m06::Reply::Rejected;
    }
    if (it->second.task) {
        return same_handoff(task, *it->second.task) ? m06::Reply::Accepted : m06::Reply::Rejected;
    }
    if (it->second.stage != Stage::Held) {
        return m06::Reply::Rejected;
    }
    const auto issue = check_task(config_, state_, it->second, task, now);
    if (issue != Issue::None) {
        return issue == Issue::StaleRevision ? m06::Reply::Conflict : m06::Reply::Rejected;
    }
    auto next = state_;
    auto& candidate = next.attempts.at(task.attempt);
    candidate.task = task;
    const auto* lane = lane_for(config_, task.candidate.tool.identity.station);
    const auto* link = current_link(state_.field->controllers, lane->controller, now, config_.max_field_age);
    candidate.controller = ControllerIdentity{link->device, link->session};
    if (live_task(candidate, now) != Issue::None || time() > task.handoff_at) {
        return m06::Reply::Rejected;
    }
    candidate.stage = Stage::Queued;
    candidate.carrier = Carrier::OnBelt;
    return commit(std::move(next)) ? m06::Reply::Accepted : m06::Reply::Uncertain;
}

m06::Reply ExecutionService::close_unused(const m06::AttemptId& id, std::string_view token) {
    if (!ready_) {
        return m06::Reply::Uncertain;
    }
    const auto it = state_.attempts.find(id);
    if (it == state_.attempts.end() || it->second.reservation.token != token) {
        return m06::Reply::Uncertain;
    }
    if (it->second.stage == Stage::ClosedUnused) {
        return m06::Reply::Accepted;
    }
    if (has_live_effect(it->second) || it->second.stage == Stage::ClosedFinished) {
        return m06::Reply::Uncertain;
    }
    auto next = state_;
    next.attempts.at(id).stage = Stage::ClosedUnused;
    return commit(std::move(next)) ? m06::Reply::Accepted : m06::Reply::Uncertain;
}

m06::AttemptStatus ExecutionService::status(const m06::AttemptId& id) {
    const auto it = state_.attempts.find(id);
    return !ready_ || it == state_.attempts.end() ? m06::AttemptStatus::Unknown : status_of(it->second.stage);
}

Report ExecutionService::ingest(const Feedback& feedback) {
    const auto now = time();
    if (!ready_) {
        return report(feedback.attempt, Issue::NotReady, feedback.request);
    }
    auto next = state_;
    const auto it = next.attempts.find(feedback.attempt);
    const auto issue = it == next.attempts.end() ? Issue::BadIdentity
                     : reduce_feedback(it->second, feedback, config_, field_of(next), now);
    if (issue == Issue::Duplicate) {
        return report(feedback.attempt, issue, feedback.request);
    }
    if (issue == Issue::BadIdentity || issue == Issue::EvidenceConflict) {
        if (next.historical.size() >= config_.max_events_per_attempt) {
            next.historical.erase(next.historical.begin());
        }
        next.historical.push_back(feedback);
    }
    if (commit(std::move(next))) {
        if (feedback.request) {
            const auto found = state_.attempts.find(feedback.attempt);
            if (found != state_.attempts.end()) {
                const auto intent = found->second.intents.find(feedback.request->operation);
                if (intent != found->second.intents.end() && intent->second.progress != Progress::Armed) {
                    armed_here_.erase(*feedback.request);
                }
            }
        }
        return report(feedback.attempt, issue, feedback.request);
    }
    return report(feedback.attempt, Issue::PersistenceUnavailable, feedback.request);
}

Report ExecutionService::request_cancel(const m06::AttemptId& id) {
    if (!ready_) {
        return report(id, Issue::NotReady);
    }
    const auto found = state_.attempts.find(id);
    if (found == state_.attempts.end()) {
        return report(id, Issue::BadIdentity);
    }
    if (found->second.cancellation_requested || closed(found->second.stage)) {
        return report(id, Issue::Duplicate);
    }
    auto next = state_;
    auto& r = next.attempts.at(id);
    r.cancellation_requested = true;
    // This process can prove it never used a locally armed command.
    for (auto& [op, intent] : r.intents) {
        if (intent.progress == Progress::Armed && armed_here_.contains(intent.wire.command.request)) {
            intent.progress = Progress::NotSent;
        }
    }
    if (!has_live_effect(r)) {
        r.stage = Stage::ClosedUnused;
    }
    if (!commit(std::move(next))) {
        return report(id, Issue::PersistenceUnavailable);
    }
    std::erase_if(armed_here_, [&](const auto& command) { return command.attempt == id; });
    return report(id, Issue::None);
}

Report ExecutionService::record_issue(const m06::AttemptId& id, Issue issue, bool block) {
    auto next = state_;
    auto& attempt = next.attempts.at(id);
    attempt.issue = issue;
    if (block || has_live_effect(attempt)) {
        attempt.needs_query = true;
        attempt.stage = Stage::Pending;
    } else {
        attempt.stage = Stage::ClosedUnused;
    }
    return report(id, commit(std::move(next)) ? issue : Issue::PersistenceUnavailable);
}

Report ExecutionService::arm(const m06::AttemptId& id, Operation op) {
    const auto now = time();
    const auto& before = state_.attempts.at(id);
    const auto gate = send_gate(config_, state_, before, op, now);
    if (gate != Issue::None) {
        if (gate == Issue::NotReady || (op == Operation::Cancel && gate == Issue::FieldBlocked)) {
            return report(id, gate);
        }
        return record_issue(id, gate, false);
    }
    if (op != Operation::Cancel) {
        const auto issue = live_task(before, now);
        if (issue != Issue::None) {
            return record_issue(id, issue, false);
        }
    }
    const auto command = command_for(before, op);
    if (!command || before.intents.contains(op)) {
        return record_issue(id, Issue::IdempotencyConflict, true);
    }
    std::vector<std::byte> bytes;
    try {
        bytes = controller_.encode(*command);
    } catch (...) {
    }
    if (bytes.empty() || bytes.size() > config_.max_wire_bytes) {
        return record_issue(id, Issue::InvalidInput, false);
    }
    auto next = state_;
    auto& r = next.attempts.at(id);
    r.intents.emplace(op, Intent{{*command, std::move(bytes)}, Progress::Armed, time(), {}});
    if (op == Operation::Prepare) {
        r.stage = Stage::Preparing;
    } else if (op != Operation::Cancel) {
        r.stage = Stage::AwaitingReceipt;
    }
    if (!commit(std::move(next))) {
        return report(id, Issue::PersistenceUnavailable, command->request);
    }
    armed_here_.insert(command->request);
    return report(id, Issue::None, command->request);
}

Report ExecutionService::transmit(const m06::AttemptId& id, Operation operation) {
    const auto& before = state_.attempts.at(id);
    const auto& intent = before.intents.at(operation);
    const auto key = intent.wire.command.request;
    auto now = time();
    auto issue = send_gate(config_, state_, before, operation, now);
    if (issue == Issue::None && operation != Operation::Cancel) {
        issue = live_task(before, now);
        now = time();
        if (issue == Issue::None) {
            issue = send_gate(config_, state_, before, operation, now);
        }
    }
    if (issue == Issue::NotReady) {
        return report(id, issue, key);
    }
    // Consume the local authorization before invoking the transport.
    armed_here_.erase(key);
    Transport sent = Transport::DefinitelyNotSent;
    if (issue == Issue::None) {
        try {
            sent = controller_.transmit(intent.wire);
        } catch (...) {
            sent = Transport::Uncertain;
        }
    }
    auto next = state_;
    auto& r = next.attempts.at(id);
    apply_transport(r, operation, sent, now);
    if (issue != Issue::None) {
        r.issue = issue;
    }
    const auto result = r.issue;
    return report(id, commit(std::move(next)) ? result : Issue::PersistenceUnavailable, key);
}

Report ExecutionService::query(const m06::AttemptId& id) {
    const auto now = time();
    const auto& before = state_.attempts.at(id);
    const auto* original = original_request(before);
    if (!original || (before.last_query && now - *before.last_query < config_.query_interval)) {
        return report(id, Issue::NotReady);
    }
    const auto request = original->wire.command.request;
    auto next = state_;
    auto& r = next.attempts.at(id);
    Issue result = Issue::QueryUnresolved;
    try {
        const auto reply = controller_.query(original->wire.command.controller, request);
        result = merge_query(r, reply, config_, field_of(next), time());
    } catch (...) {
        r.last_query = now;
    }
    if (result == Issue::QueryUnresolved && r.issue != Issue::EvidenceConflict) {
        r.issue = result;
    }
    return report(id, commit(std::move(next)) ? result : Issue::PersistenceUnavailable, request);
}

Report ExecutionService::step(const m06::AttemptId& id) {
    const auto& r = state_.attempts.at(id);
    const auto now = time();
    if (closed(r.stage)) {
        return report(id, Issue::NotReady);
    }
    if (r.stage == Stage::Held && now >= r.reservation.valid_until) {
        return record_issue(id, Issue::Expired, false);
    }
    for (const auto& [op, intent] : r.intents) {
        if (intent.progress == Progress::Armed && armed_here_.contains(intent.wire.command.request)) {
            return transmit(id, op);
        }
    }
    if (const auto next = next_operation(r)) {
        const auto result = arm(id, *next);
        if (*next != Operation::Cancel || result.issue != Issue::FieldBlocked) {
            return result;
        }
    }
    if (r.needs_query) {
        return query(id);
    }
    if (r.task) {
        const auto receive = applicable_receipt(*r.task);
        const auto latest_occupancy = std::ranges::max_element(r.reservation.uses, {}, [](const auto& use) {
            return use.time.end;
        })->time.end;
        const bool receipt_expired = (r.stage == Stage::AwaitingReceipt || r.stage == Stage::Preparing) &&
                                     (!receive || now > receive->close);
        const bool contact_expired = r.stage == Stage::Received && now > r.task->contact.instants.close;
        const bool occupancy_expired = (r.stage == Stage::Returning || r.stage == Stage::Active) &&
                                       now >= latest_occupancy;
        if (receipt_expired || contact_expired || occupancy_expired) {
            return record_issue(id, Issue::Expired, true);
        }
    }
    return report(id, Issue::NotReady);
}

std::vector<Report> ExecutionService::advance() {
    std::vector<Report> result;
    std::vector<m06::AttemptId> attempts;
    for (const auto& [id, record] : state_.attempts) {
        if (!closed(record.stage)) {
            attempts.push_back(id);
        }
    }
    for (const auto& id : attempts) {
        if (!ready_) {
            break;
        }
        auto step_result = step(id);
        if (step_result.issue != Issue::NotReady) {
            result.push_back(std::move(step_result));
        }
    }
    return result;
}

}  // namespace waste::m07
