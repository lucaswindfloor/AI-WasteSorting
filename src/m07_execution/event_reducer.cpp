#include "event_reducer.h"

#include <algorithm>
#include <utility>

namespace waste::m07 {
namespace {

bool same_plan(const std::optional<m06::PlanEnvelope>& a, const std::optional<m06::PlanEnvelope>& b) {
    return a.has_value() == b.has_value() && (!a ||
        (a->attempt == b->attempt && a->target == b->target && a->plan_id == b->plan_id &&
         a->reservation_token == b->reservation_token &&
         a->valid_for_receipt_until == b->valid_for_receipt_until && a->expected_contact == b->expected_contact));
}

bool same_event(const Feedback& a, const Feedback& b) {
    return a.attempt == b.attempt && a.request == b.request && a.stamp == b.stamp &&
           a.kind == b.kind && same_plan(a.plan, b.plan);
}

bool same_stream(const Stamp& a, const Stamp& b) {
    return a.source == b.source && a.device == b.device && a.session == b.session;
}

bool from_current_link(const Stamp& stamp, const FieldState* field, Tick now, Tick max_age) {
    if (!field) {
        return false;
    }
    const auto& links = stamp.source == Source::Controller ? field->controllers : field->terminals;
    const auto* link = current_link(links, stamp.device, now, max_age);
    return link && link->session == stamp.session;
}

bool effect(Operation o) { return o == Operation::Commit || o == Operation::FinalGoal; }

bool can_prove_effect(const AttemptRecord& record, const Intent& intent, bool history, Tick occurred) {
    return intent.progress != Progress::NotSent &&
           (intent.progress != Progress::Armed || history || record.needs_query) &&
           occurred >= intent.sent_at.value_or(intent.armed_at);
}

bool valid_event(const AttemptRecord& r, const Feedback& e, const Lane& lane,
                 const FieldState* field, Tick now, Tick max_age, bool history) {
    const auto& s = e.stamp;
    if (!r.task || !r.controller || e.attempt != r.reservation.attempt ||
        !s.sequence || s.session.empty() || s.evidence.empty() || s.occurred < r.task->handoff_at ||
        s.occurred > now || s.valid_until <= s.occurred ||
        (e.kind == EventKind::PlanReady) != e.plan.has_value() ||
        (!history && !from_current_link(s, field, now, max_age))) {
        return false;
    }
    if (s.source == Source::Terminal) {
        if (s.device != lane.terminal || e.request) {
            return false;
        }
        const auto execution = r.intents.find(r.task->contract == m06::Contract::A
                                               ? Operation::Commit : Operation::FinalGoal);
        return execution != r.intents.end() && can_prove_effect(r, execution->second, history, s.occurred) &&
               (e.kind == EventKind::Holding || e.kind == EventKind::Released ||
                e.kind == EventKind::ToolEmpty || e.kind == EventKind::ToolHealthy ||
                e.kind == EventKind::ToolUnavailable);
    }
    if (s.source != Source::Controller || s.device != lane.controller || !e.request ||
        e.request->attempt != e.attempt ||
        (s.session != r.controller->session && e.kind != EventKind::RobotReady)) {
        return false;
    }
    const auto op = e.request->operation;
    const auto intent = r.intents.find(op);
    if (intent == r.intents.end() || !can_prove_effect(r, intent->second, history, s.occurred)) {
        return false;
    }
    switch (e.kind) {
    case EventKind::Receipt: return true;
    case EventKind::PlanReady:
        return op == Operation::Prepare && e.plan->attempt == r.reservation.attempt &&
               e.plan->target == r.task->candidate.target && !e.plan->plan_id.empty() &&
               e.plan->reservation_token == r.reservation.token && m06::valid(e.plan->expected_contact);
    case EventKind::Admitted:
    case EventKind::Approaching:
    case EventKind::MotionObserved:
    case EventKind::PlacementEnded:
    case EventKind::Returning:
    case EventKind::Finished: return effect(op);
    case EventKind::NoAction:
    case EventKind::RobotReady: {
        const auto commit = r.intents.find(Operation::Commit);
        const bool actionable_commit = commit != r.intents.end() && commit->second.progress != Progress::NotSent;
        return effect(op) || (op == Operation::Prepare && !actionable_commit);
    }
    case EventKind::CancelAcknowledged: return op == Operation::Cancel;
    case EventKind::Holding:
    case EventKind::Released:
    case EventKind::ToolEmpty:
    case EventKind::ToolHealthy:
    case EventKind::ToolUnavailable: break;
    }
    return false;
}

const Feedback* fact(const AttemptRecord& r, EventKind kind) {
    const auto it = r.facts.find(kind);
    return it == r.facts.end() ? nullptr : &it->second;
}

bool motion(EventKind kind) {
    return kind == EventKind::Approaching || kind == EventKind::MotionObserved || kind == EventKind::Holding || kind == EventKind::PlacementEnded ||
           kind == EventKind::Released || kind == EventKind::Returning || kind == EventKind::Finished;
}

bool usable(const Feedback* feedback, Tick after, const FieldState* field, Tick now,
            Tick max_event_age, Tick max_field_age) {
    if (!feedback) {
        return false;
    }
    const auto& stamp = feedback->stamp;
    return stamp.occurred >= after && stamp.occurred <= now && now < stamp.valid_until &&
           now - stamp.occurred < max_event_age && from_current_link(stamp, field, now, max_field_age);
}

}  // namespace

void refresh_stage(AttemptRecord& r, const ExecutionConfig& c, const FieldState* f, Tick now) {
    if (closed(r.stage)) {
        return;
    }
    const auto* hold = fact(r, EventKind::Holding);
    const auto* release = fact(r, EventKind::Released);
    if (hold && (!release || hold->stamp.occurred >= release->stamp.occurred)) {
        r.carrier = Carrier::Held;
    } else if (release) {
        r.carrier = Carrier::Released;
    }
    const auto* finished = fact(r, EventKind::Finished);
    const auto* unused = fact(r, EventKind::NoAction);
    if (r.motion_seen && unused) {
        r.needs_query = true;
        r.issue = Issue::EvidenceConflict;
    }
    const bool completed = finished && r.motion_seen && !unused;
    const bool no_motion = unused && !r.motion_seen;
    const auto* terminal = completed ? finished : no_motion ? unused : nullptr;
    if (terminal && r.carrier != Carrier::Held && r.issue != Issue::EvidenceConflict) {
        auto after = terminal->stamp.occurred;
        for (const auto& [kind, event] : r.facts) {
            if (motion(kind)) {
                after = std::max(after, event.stamp.occurred);
            }
        }
        const auto* healthy = fact(r, EventKind::ToolHealthy);
        const auto* unavailable = fact(r, EventKind::ToolUnavailable);
        const bool only_prepare = no_motion && unused->request &&
                                  unused->request->operation == Operation::Prepare;
        if (usable(fact(r, EventKind::RobotReady), after, f, now, c.max_event_age, c.max_field_age) &&
            (only_prepare || (usable(fact(r, EventKind::ToolEmpty), after, f, now, c.max_event_age, c.max_field_age) &&
                            usable(healthy, after, f, now, c.max_event_age, c.max_field_age) &&
                            (!unavailable || healthy->stamp.occurred > unavailable->stamp.occurred)))) {
            r.stage = completed ? Stage::ClosedFinished : Stage::ClosedUnused;
            r.needs_query = false;
            r.issue = Issue::None;
            return;
        }
    }
    if (r.needs_query) {
        r.stage = Stage::Pending;
    } else if (finished || fact(r, EventKind::Returning) || release) {
        r.stage = Stage::Returning;
    } else if (r.motion_seen || fact(r, EventKind::Admitted)) {
        r.stage = Stage::Active;
    }
}

Issue reduce_feedback(AttemptRecord& r, const Feedback& event, const ExecutionConfig& c,
                      const FieldState* field, Tick now, bool history) {
    if (!r.task || closed(r.stage)) {
        return Issue::BadIdentity;
    }
    const auto* lane = lane_for(c, r.task->candidate.tool.identity.station);
    if (!lane || !valid_event(r, event, *lane, field, now, c.max_field_age, history)) {
        return Issue::BadIdentity;
    }
    for (const auto& previous : r.recent_events) {
        if (same_stream(previous.stamp, event.stamp) && previous.stamp.sequence == event.stamp.sequence) {
            if (same_event(previous, event)) {
                return Issue::Duplicate;
            }
            r.needs_query = true;
            r.issue = Issue::EvidenceConflict;
            refresh_stage(r, c, field, now);
            return r.issue;
        }
    }
    const auto* previous = fact(r, event.kind);
    if (previous && same_stream(previous->stamp, event.stamp) &&
        (previous->stamp.sequence >= event.stamp.sequence || previous->stamp.occurred > event.stamp.occurred)) {
        return Issue::Duplicate;
    }
    if (r.recent_events.size() >= c.max_events_per_attempt) {
        r.recent_events.erase(r.recent_events.begin());
    }
    r.recent_events.push_back(event);
    r.facts[event.kind] = event;

    if (event.request && event.kind != EventKind::RobotReady) {
        auto& intent = r.intents.at(event.request->operation);
        const bool actionable = effect(event.request->operation);
        intent.progress = Progress::Received;
        if (event.kind == EventKind::Receipt && actionable) {
            if (event.stamp.occurred < intent.wire.command.payload.receive.open ||
                event.stamp.occurred > intent.wire.command.payload.receive.close) {
                r.needs_query = true;
                r.issue = Issue::Expired;
            } else if (r.stage == Stage::AwaitingReceipt) {
                r.stage = Stage::Received;
            }
        }
    }
    if (event.plan) {
        if (r.plan && !same_plan(r.plan, event.plan)) {
            r.needs_query = true;
            r.issue = Issue::EvidenceConflict;
        } else {
            r.plan = event.plan;
            if (r.stage == Stage::Preparing) {
                r.stage = Stage::PlanReady;
            }
        }
    }
    if (event.kind == EventKind::CancelAcknowledged) {
        r.needs_query = true;
    }
    r.motion_seen |= motion(event.kind);
    refresh_stage(r, c, field, now);
    return Issue::None;
}

}  // namespace waste::m07
