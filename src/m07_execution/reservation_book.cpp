#include "reservation_book.h"
#include "m06_scheduling/resource_schedule.h"

#include <algorithm>
#include <set>

namespace waste::m07 {
namespace {

bool overlaps(m06::Interval a, m06::Interval b) {
    return a.begin < b.end && b.begin < a.end;
}

bool covers(m06::Interval outer, m06::Interval inner) {
    return outer.begin <= inner.begin && outer.end >= inner.end;
}

bool valid_kind(m06::ResourceKind k) {
    return k == m06::ResourceKind::Robot || k == m06::ResourceKind::Tool ||
           k == m06::ResourceKind::Bin || k == m06::ResourceKind::E11;
}

}  // namespace

bool closed(Stage s) { return s == Stage::ClosedUnused || s == Stage::ClosedFinished; }

bool same_reservation_request(const m06::ReservationRequest& a, const m06::ReservationRequest& b) {
    return a.attempt == b.attempt && a.target == b.target && a.expected_revision == b.expected_revision &&
           a.uses == b.uses && a.handoff_at == b.handoff_at && a.handoff_deadline == b.handoff_deadline;
}

bool has_live_effect(const AttemptRecord& r) {
    return r.motion_seen || std::ranges::any_of(r.intents, [](const auto& item) {
        return item.second.progress != Progress::NotSent;
    });
}

m06::AttemptStatus status_of(Stage stage) {
    using Result = m06::AttemptStatus;
    switch (stage) {
    case Stage::Held: return Result::Held;
    case Stage::Queued:
    case Stage::Preparing:
    case Stage::PlanReady:
    case Stage::AwaitingReceipt:
    case Stage::Received: return Result::Delivered;
    case Stage::Active:
    case Stage::Returning: return Result::Active;
    case Stage::Pending: return Result::Unknown;
    case Stage::ClosedUnused: return Result::ClosedUnused;
    case Stage::ClosedFinished: return Result::ClosedFinished;
    }
    return Result::Unknown;
}

m06::ExecutionSnapshot build_snapshot(const DurableState& state, const ExecutionConfig& config, Tick now) {
    m06::ExecutionSnapshot result;
    if (state.field) {
        result.eligibility = state.field->eligibility;
        result.calendars = state.field->calendars;
    }
    result.eligibility.generation = state.generation;
    result.eligibility.revision = state.revision;
    if (!gate_open(config, state.field ? &*state.field : nullptr, now)) {
        for (auto& calendar : result.calendars) {
            calendar.usable.clear();
        }
    }
    for (const auto& [id, r] : state.attempts) {
        if (r.stage == Stage::ClosedUnused) {
            continue;
        }
        result.unresolved_targets.push_back(id.target);
        if (closed(r.stage)) {
            continue;
        }
        result.reservations.push_back(r.reservation);
        // Scheduled periods alone cannot prove readiness after an operation
        // has escaped. Block the physical resources until feedback closes it.
        if (r.stage == Stage::Pending || has_live_effect(r)) {
            for (const auto& use : r.reservation.uses) {
                auto it = std::ranges::find(result.calendars, use.resource, &m06::ResourceCalendar::resource);
                if (it != result.calendars.end()) {
                    it->occupied.push_back({now, Tick::max()});
                }
            }
        }
    }
    return result;
}

Issue check_reservation(const DurableState& state, const ExecutionConfig& c,
                        const m06::ReservationRequest& request, Tick now) {
    if (request.expected_revision != state.revision) {
        return Issue::StaleRevision;
    }
    if (request.target.generation != c.generation || request.attempt.target != m06::target_key(request.target) ||
        !request.target.id || !request.target.version || request.attempt.scheduler_session.empty() ||
        !request.attempt.sequence || request.uses.empty() || request.uses.size() > c.max_resources ||
        request.handoff_at < now || request.handoff_deadline < request.handoff_at ||
        request.handoff_at >= c.valid_until || request.handoff_deadline >= Tick::max()) {
        return Issue::InvalidInput;
    }
    if (!gate_open(c, state.field ? &*state.field : nullptr, now)) {
        return Issue::FieldBlocked;
    }
    const auto current = build_snapshot(state, c, now);
    if (m06::target_in_use(current, request.attempt.target)) {
        return Issue::ResourceConflict;
    }
    std::set<m06::ResourceKey> distinct;
    for (const auto& use : request.uses) {
        if (!valid_kind(use.resource.kind) || use.resource.id.empty() ||
            !distinct.insert(use.resource).second || !m06::valid(use.time) ||
            use.time.begin < request.handoff_at) {
            return Issue::InvalidInput;
        }
        const auto it = std::ranges::find(current.calendars, use.resource, &m06::ResourceCalendar::resource);
        if (it == current.calendars.end() || now >= it->valid_until || request.handoff_at >= it->valid_until ||
            !std::ranges::any_of(it->usable, [&](auto region) { return covers(region, use.time); }) ||
            std::ranges::any_of(it->occupied, [&](auto busy) { return overlaps(busy, use.time); })) {
            return Issue::ResourceConflict;
        }
        for (const auto& r : current.reservations) {
            if (std::ranges::any_of(r.uses, [&](const auto& other) {
                return other.resource == use.resource && overlaps(other.time, use.time);
            })) {
                return Issue::ResourceConflict;
            }
        }
    }
    return Issue::None;
}

}  // namespace waste::m07
