#include "scheduling_service.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace waste::m06 {
namespace {

std::optional<ExecutionSnapshot> read_snapshot(ExecutionPort& port) {
    try {
        return port.snapshot();
    } catch (...) {
        return {};
    }
}

std::shared_ptr<const CurrentTarget> read_target(TargetSource& source, const TargetKey& key, Tick now) {
    try {
        return source.read(key, now);
    } catch (...) {
        return {};
    }
}

}  // namespace

SchedulingService::SchedulingService(SchedulingConfig config, const m05::EligibilityService& eligibility,
                                     Clock clock)
    : config_(std::move(config)), eligibility_(eligibility), clock_(std::move(clock)),
      attempts_(config_.scheduler_session, config_.max_attempts) {
    validate(config_);
}

Tick SchedulingService::update_time(Tick supplied) {
    now_ = std::max({supplied, now_, clock_ ? clock_() : supplied});
    return now_;
}

Reason SchedulingService::close_unused(ExecutionPort& execution, Attempt& attempt, Reason original) {
    try {
        if (execution.close_unused(attempt.id, attempt.token) == Reply::Accepted) {
            const auto target = attempt.id.target;
            attempts_.settle(target);
            return original;
        }
    } catch (...) {
    }
    attempt.phase = AttemptPhase::Uncertain;
    return Reason::EffectUncertain;
}

Reason SchedulingService::reserve_and_handoff(const Selection& chosen, const ExecutionSnapshot& snapshot,
                                               ExecutionPort& execution, TargetSource& targets,
                                               Decision& decision, DispatchReport& report) {
    if (update_time(now_) > chosen.handoff_at) {
        return Reason::ReservationConflict;
    }
    const auto key = target_key(chosen.candidate.target);
    auto* attempt = attempts_.open(key, chosen.candidate.valid_until);
    if (!attempt) {
        return attempts_.find(key) ? Reason::ActiveAttempt : Reason::Capacity;
    }
    decision.attempt = attempt->id;
    const ReservationRequest request{attempt->id, chosen.candidate.target, snapshot.eligibility.revision,
                                     chosen.uses, chosen.handoff_at, chosen.deadlines.handoff.close};
    ReserveReply reply;
    try {
        reply = execution.reserve(request);
    } catch (...) {
        attempt->phase = AttemptPhase::Uncertain;
        return Reason::EffectUncertain;
    }
    if ((reply.status == Reply::Conflict || reply.status == Reply::Rejected) && !reply.reservation) {
        attempts_.settle(key);
        return reply.status == Reply::Conflict ? Reason::ReservationConflict : Reason::ReservationRejected;
    }
    if (reply.status != Reply::Accepted || !reply.reservation || reply.reservation->attempt != request.attempt ||
        reply.reservation->token.empty()) {
        attempt->phase = AttemptPhase::Uncertain;
        return Reason::EffectUncertain;
    }
    const auto& reservation = *reply.reservation;
    attempt->phase = AttemptPhase::Reserved;
    attempt->token = reservation.token;
    if (reservation.uses != request.uses) {
        return close_unused(execution, *attempt, Reason::ReservationMismatch);
    }
    if (update_time(now_) >= reservation.valid_until || request.handoff_at >= reservation.valid_until) {
        return close_unused(execution, *attempt, Reason::ReservationExpired);
    }

    const auto fresh = read_snapshot(execution);
    if (!fresh || fresh->eligibility.revision < request.expected_revision || !snapshot_valid(config_, *fresh)) {
        return close_unused(execution, *attempt, Reason::SnapshotUnavailable);
    }
    if (std::ranges::find(fresh->reservations, reservation) == fresh->reservations.end()) {
        return close_unused(execution, *attempt, Reason::ReservationMismatch);
    }
    const auto current = read_target(targets, key, update_time(now_));
    const auto updated = current ? refresh_candidate(eligibility_, chosen.candidate, *current,
                                                      fresh->eligibility, update_time(now_), true)
                                 : std::nullopt;
    if (!updated) {
        return close_unused(execution, *attempt, Reason::RecheckFailed);
    }
    const QueuedCandidate entry{*updated, chosen.priority, chosen.arrival_sequence};
    const auto fit = evaluate_candidate(config_, entry, *current, *updated, *fresh, update_time(now_),
                                        chosen.handoff_at, reservation.token);
    if (!fit.selected) {
        return close_unused(execution, *attempt, fit.reason);
    }
    const auto& final = *fit.selected;
    if (final.uses != reservation.uses || final.lane != chosen.lane) {
        return close_unused(execution, *attempt, Reason::ReservationMismatch);
    }
    decision.contact = final.contact;
    decision.deadlines = final.deadlines;

    const bool native = final.lane->tracking == TrackingMode::ControllerNative;
    auto deadlines = final.deadlines;
    deadlines.handoff.close = std::min(deadlines.handoff.close, reservation.valid_until - Tick(1));
    Task task{attempt->id, reservation, final.candidate, config_.version, final.forecast_version,
              final.lane->tracking, final.lane->contract,
              native ? *final.candidate.contact.center : final.contact.current_pick,
              native ? final.candidate.target.reference : final.contact.current_reference,
              native ? 0 : current->prediction.applied_mm, final.contact, deadlines,
              final.handoff_at, fresh->eligibility.revision};
    const auto ready = update_time(now_);
    if (ready > task.handoff_at || ready >= task.candidate.valid_until || ready >= reservation.valid_until) {
        return close_unused(execution, *attempt, Reason::ReceiveDeadline);
    }
    Reply handoff{Reply::Uncertain};
    try {
        handoff = execution.handoff(task);
    } catch (...) {
    }
    if (handoff == Reply::Accepted) {
        attempt->phase = AttemptPhase::HandedOff;
        report.task = std::move(task);
        return Reason::None;
    }
    if (handoff == Reply::Rejected || handoff == Reply::Conflict) {
        return close_unused(execution, *attempt, Reason::HandoffRejected);
    }
    attempt->phase = AttemptPhase::Uncertain;
    return Reason::EffectUncertain;
}

DispatchReport SchedulingService::dispatch(std::span<const QueuedCandidate> batch, ExecutionPort& execution,
                                            TargetSource& targets, Tick now) {
    if (now < Tick::zero()) {
        throw std::invalid_argument("scheduling clock must be nonnegative");
    }
    DispatchReport report;
    const auto global = [&](Reason reason) {
        report.decisions.push_back({{config_.generation.run_id, 0}, reason, {}, {}, {}});
    };
    update_time(now);
    if (batch.size() > config_.max_batch) {
        global(Reason::Capacity);
        return report;
    }
    attempts_.reconcile(execution, update_time(now_));
    for (std::size_t retry = 0; retry <= config_.max_retries; ++retry) {
        const auto snapshot = read_snapshot(execution);
        if (!snapshot || !snapshot_valid(config_, *snapshot)) {
            global(Reason::SnapshotUnavailable);
            return report;
        }
        std::optional<Selection> chosen;
        for (const auto& entry : batch) {
            const auto key = target_key(entry.candidate.target);
            const auto reject = [&](Reason reason) {
                report.decisions.push_back({key, reason, {}, {}, {}});
            };
            if (entry.candidate.target.generation != config_.generation) {
                reject(Reason::WrongGeneration);
                continue;
            }
            if (attempts_.find(key) || target_in_use(*snapshot, key)) {
                reject(Reason::ActiveAttempt);
                continue;
            }
            const auto current = read_target(targets, key, update_time(now_));
            const auto fresh = current ? refresh_candidate(eligibility_, entry.candidate, *current,
                                                           snapshot->eligibility, update_time(now_), false)
                                       : std::nullopt;
            if (!fresh) {
                reject(Reason::CandidateStale);
                continue;
            }
            auto evaluated = evaluate_candidate(config_, entry, *current, *fresh, *snapshot, update_time(now_));
            if (!evaluated.selected) {
                reject(evaluated.reason);
            } else if (!chosen || preferred(*evaluated.selected, *chosen)) {
                chosen = std::move(*evaluated.selected);
            }
        }
        if (!chosen) {
            break;
        }
        Decision decision{target_key(chosen->candidate.target), Reason::None, {}, chosen->contact, chosen->deadlines};
        decision.reason = reserve_and_handoff(*chosen, *snapshot, execution, targets, decision, report);
        const auto retryable = decision.reason == Reason::ReservationConflict;
        report.decisions.push_back(std::move(decision));
        if (!retryable) {
            break;
        }
    }
    return report;
}

}  // namespace waste::m06
