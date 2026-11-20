#include "m07_intent_store_adapter.h"
#include "checkpoint_layouts.h"

namespace waste::m09 {
namespace {

constexpr std::string_view schema = "m07-execution-v1";

std::string id(const m06::AttemptId& attempt) {
    return wire::key({attempt.target.run_id, std::to_string(attempt.target.id),
                      attempt.scheduler_session, std::to_string(attempt.sequence)});
}

m07::StoreStatus store_result(Status status) {
    if (accepted(status)) return m07::StoreStatus::Confirmed;
    if (status == Status::Conflict) return m07::StoreStatus::Conflict;
    if (status == Status::Invalid || status == Status::Capacity) return m07::StoreStatus::Rejected;
    return m07::StoreStatus::Uncertain;
}

Record make_record(const m07::DurableState& state, RecordKey key, std::string description) {
    Record record;
    record.run = state.generation.run_id;
    record.epoch = state.generation.config_epoch;
    record.key = std::move(key);
    record.description = std::move(description);
    return record;
}

}  // namespace

RecordKey execution_attempt_key(const m06::AttemptId& attempt) { return {Kind::Attempt, id(attempt), 1}; }

RecordKey execution_intent_key(const m07::ControllerIdentity& controller, const m07::RequestId& request) {
    return {Kind::Intent, wire::key({controller.device, controller.session, id(request.attempt),
                                    std::to_string(static_cast<unsigned>(request.operation))}), 1};
}

M07IntentStore::M07IntentStore(ArchiveService& archive, std::string station, std::string run)
    : archive_(archive), run_(std::move(run)), station_(std::move(station)),
      journal_(archive.root() / "critical" / "m07" / wire::key({station_, run_})) {
    if (!wire::identifier(station_) || !wire::identifier(run_)) throw std::invalid_argument("invalid M07 scope");
}

Result<m07::DurableState> M07IntentStore::checkpoint(std::uint64_t revision) const {
    const auto stored = journal_.load(revision);
    if (!stored.value) return {stored.status, {}};
    auto state = wire::decode<m07::DurableState>(schema, stored.value->state);
    if (!state || state->generation.run_id != run_ || state->revision != stored.value->revision ||
        state->transaction != stored.value->transaction) return {Status::Corrupt, {}};
    return {Status::Ok, std::move(state)};
}

m07::LoadReply M07IntentStore::load() {
    auto value = checkpoint();
    if (!value.value) return {value.status == Status::Missing ? m07::LoadStatus::Empty : m07::LoadStatus::Unavailable, {}};
    return {m07::LoadStatus::Restored, std::move(value.value)};
}

m07::CommitReply M07IntentStore::commit(std::uint64_t expected, const m07::DurableState& next) {
    if (next.generation.run_id != run_) return {m07::StoreStatus::Rejected, 0, {}};
    Checkpoint saved{next.revision, next.transaction, {}};
    for (const auto& [attempt, record] : next.attempts) {
        if (attempt.target.run_id != run_ || record.original.attempt != attempt) {
            return {m07::StoreStatus::Rejected, 0, {}};
        }
        for (const auto& [operation, intent] : record.intents) {
            const auto& command = intent.wire.command;
            if (command.request.attempt != attempt || command.request.operation != operation ||
                command.payload.target.generation.run_id != run_ || intent.wire.bytes.empty() ||
                !wire::identifier(command.controller.device) || !wire::identifier(command.controller.session)) {
                return {m07::StoreStatus::Rejected, 0, {}};
            }
            const auto key = execution_intent_key(command.controller, command.request);
            saved.intent_digests.emplace(key.id, wire::digest(wire::encode("m07-wire-v1", intent.wire)));
        }
    }
    saved.state = wire::encode(schema, next);
    const auto status = journal_.commit(expected, saved);
    return {store_result(status), accepted(status) ? next.revision : 0,
            accepted(status) ? next.transaction : std::string{}};
}

Result<m07::Intent> M07IntentStore::lookup(const m07::ControllerIdentity& controller,
                                          const m07::RequestId& request) const {
    if (request.attempt.target.run_id != run_) return {Status::Invalid, {}};
    auto state = checkpoint();
    while (state.value) {
        const auto attempt = state.value->attempts.find(request.attempt);
        if (attempt != state.value->attempts.end()) {
            const auto intent = attempt->second.intents.find(request.operation);
            if (intent != attempt->second.intents.end() && intent->second.wire.command.controller == controller &&
                intent->second.wire.command.request == request) return {Status::Ok, intent->second};
        }
        if (state.value->revision <= 1) return {Status::Missing, {}};
        state = checkpoint(state.value->revision - 1);
    }
    return {state.status, {}};
}

Result<Manifest> M07IntentStore::publish(std::uint64_t revision) {
    const auto saved = checkpoint(revision);
    if (!saved.value) return {saved.status, {}};
    const auto& state = *saved.value;
    std::vector<Record> records;
    auto snapshot = make_record(state, {Kind::Snapshot, wire::key({"m07", station_}), state.revision},
                                "M07 task, carrier and resource checkpoint");
    snapshot.detail = wire::encode(schema, state);
    for (const auto& [attempt, row] : state.attempts) {
        const auto attempt_key = execution_attempt_key(attempt);
        const auto target = std::to_string(attempt.target.id);
        if (row.task) {
            auto task = make_record(state, attempt_key, "M07 accepted grasp task");
            task.epoch = row.task->candidate.target.generation.config_epoch;
            task.target = target;
            task.planned_bin = row.task->candidate.bin.identity.slot;
            task.related.push_back({Kind::Target, target, row.task->candidate.target.version});
            task.detail = wire::encode("m06-task-v1", *row.task);
            snapshot.related.push_back(task.key);
            records.push_back(std::move(task));
        }
        for (const auto& [operation, intent] : row.intents) {
            const auto& command = intent.wire.command;
            auto item = make_record(state, execution_intent_key(command.controller, command.request),
                                    "Recoverable M07 command intent");
            item.epoch = command.payload.target.generation.config_epoch;
            item.target = target;
            item.planned_bin = command.payload.bin.slot;
            item.related.push_back(attempt_key);
            item.detail = wire::encode("m07-wire-v1", intent.wire);
            snapshot.related.push_back(item.key);
            records.push_back(std::move(item));
        }
    }
    // Original facts are kept in the checkpoint. The per-event records are
    // indexed using the producer session, not the current controller session.
    const auto append_feedback = [&](const m07::Feedback& feedback) {
        const auto& stamp = feedback.stamp;
        auto event = make_record(state, {Kind::Execution,
            wire::key({std::to_string(static_cast<unsigned>(stamp.source)), stamp.device, stamp.session,
                       std::to_string(stamp.sequence)}), 1}, "M07 observed execution fact");
        event.epoch = 0; // May be an event from an earlier execution generation.
        event.target = std::to_string(feedback.attempt.target.id);
        event.related.push_back(execution_attempt_key(feedback.attempt));
        if (feedback.request && stamp.source == m07::Source::Controller) {
            event.related.push_back(execution_intent_key({stamp.device, stamp.session}, *feedback.request));
        }
        event.detail = wire::encode("m07-feedback-v1", feedback);
        records.push_back(std::move(event));
    };
    for (const auto& feedback : state.historical) append_feedback(feedback);
    for (const auto& [attempt, row] : state.attempts) {
        for (const auto& event : row.recent_events) append_feedback(event);
    }
    records.push_back(std::move(snapshot));
    return archive_.append(run_, records);
}

}  // namespace waste::m09
