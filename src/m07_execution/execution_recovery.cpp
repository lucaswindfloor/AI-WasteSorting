#include "execution_recovery.h"

namespace waste::m07 {

bool compatible_checkpoint(const DurableState& s, const ExecutionConfig& c, Tick now) {
    if (s.generation != c.generation || s.configuration != c.version || !s.revision || s.transaction.empty() ||
        s.updated < Tick::zero() || s.updated > now || s.attempts.size() > c.max_attempts ||
        s.historical.size() > c.max_events_per_attempt ||
        (s.field && (!valid_field(c, *s.field, now) || s.field->sequence != s.field_sequence))) {
        return false;
    }
    for (const auto& [id, a] : s.attempts) {
        if (id != a.original.attempt || id != a.reservation.attempt ||
            a.original.target.generation != c.generation || id.target != m06::target_key(a.original.target) ||
            a.reservation.token.empty() || a.reservation.uses != a.original.uses ||
            a.reservation.uses.empty() || a.reservation.uses.size() > c.max_resources ||
            a.recent_events.size() > c.max_events_per_attempt ||
            (a.task && (a.task->attempt != id || a.task->reservation != a.reservation || !a.controller)) ||
            (!a.task && (a.controller || !a.intents.empty() || (!closed(a.stage) && a.stage != Stage::Held)))) {
            return false;
        }
        for (const auto& [operation, intent] : a.intents) {
            const auto& command = intent.wire.command;
            if (!a.task || !a.controller || command.request != RequestId{id, operation} ||
                command.controller != *a.controller || command.payload != payload_from(*a.task) ||
                intent.wire.bytes.empty() || intent.wire.bytes.size() > c.max_wire_bytes) {
                return false;
            }
        }
    }
    return true;
}

void mark_restarted(DurableState& state) {
    // Current online facts must be reacquired after process startup. Retain
    // the source sequence so an old snapshot cannot re-enable dispatch.
    state.field.reset();
    for (auto& [id, attempt] : state.attempts) {
        if (!closed(attempt.stage) && has_live_effect(attempt)) {
            attempt.stage = Stage::Pending;
            attempt.needs_query = true;
            if (attempt.issue != Issue::EvidenceConflict) {
                attempt.issue = Issue::QueryUnresolved;
            }
        }
    }
}

const Intent* original_request(const AttemptRecord& r) {
    for (auto operation : {Operation::Commit, Operation::FinalGoal, Operation::Prepare}) {
        const auto found = r.intents.find(operation);
        if (found != r.intents.end() && found->second.progress != Progress::NotSent) {
            return &found->second;
        }
    }
    return nullptr;
}

Issue merge_query(AttemptRecord& r, const QueryReply& reply, const ExecutionConfig& c,
                  const FieldState* field, Tick now) {
    const auto* original = original_request(r);
    r.last_query = now;
    if (!original || reply.request != original->wire.command.request ||
        reply.original != original->wire.command.controller || !reply.proof ||
        reply.proof->source != Source::Controller || reply.proof->device != reply.original.device ||
        reply.proof->session != reply.original.session) {
        return Issue::QueryUnresolved;
    }
    std::optional<EventKind> event;
    switch (reply.state) {
    case QueryState::Acting: event = EventKind::MotionObserved; break;
    case QueryState::Finished: event = EventKind::Finished; break;
    case QueryState::TerminatedWithoutMotion: event = EventKind::NoAction; break;
    case QueryState::Unknown:
    case QueryState::Missing:
    case QueryState::Queued: break;
    }
    if (!event) {
        return Issue::QueryUnresolved;
    }
    const auto was_conflict = r.issue == Issue::EvidenceConflict;
    const auto result = reduce_feedback(r, {r.original.attempt, reply.request, *reply.proof, *event, {}},
                                       c, field, now, true);
    if (result == Issue::None || result == Issue::Duplicate) {
        if (!was_conflict && r.issue != Issue::EvidenceConflict) {
            r.needs_query = false;
            r.issue = Issue::None;
        }
        refresh_stage(r, c, field, now);
        return r.issue;
    }
    return result;
}

}  // namespace waste::m07
