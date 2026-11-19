#include "contract_timing.h"

#include <algorithm>

namespace waste::m06 {

std::optional<Deadlines> contract_deadlines(Contract contract, const Budget& b,
                                            Window contact, std::optional<Tick> receipt_expiry) {
    if ((contract != Contract::A && contract != Contract::B) ||
        b.handoff_to_receipt_min < Tick::zero() || b.handoff_to_receipt_max < b.handoff_to_receipt_min ||
        b.receipt_to_contact_min < Tick::zero() || b.receipt_to_contact_max < b.receipt_to_contact_min ||
        b.early_margin < Tick::zero() || b.late_margin < Tick::zero()) {
        return {};
    }
    const auto guarded_open = add_ticks(contact.open, b.early_margin);
    if (!valid(contact) || !guarded_open || contact.close < b.late_margin ||
        contact.close - b.late_margin < b.receipt_to_contact_max) {
        return {};
    }
    auto latest_receive = contact.close - b.late_margin - b.receipt_to_contact_max;
    if (receipt_expiry) {
        if (*receipt_expiry <= Tick::zero()) {
            return {};
        }
        latest_receive = std::min(latest_receive, *receipt_expiry - Tick(1));
    }
    const auto earliest_receive = std::max(Tick::zero(), *guarded_open - b.receipt_to_contact_min);
    const Window receive{earliest_receive, latest_receive};
    if (!valid(receive) || latest_receive < b.handoff_to_receipt_max) {
        return {};
    }
    const Window handoff{std::max(Tick::zero(), earliest_receive - b.handoff_to_receipt_min),
                         latest_receive - b.handoff_to_receipt_max};
    if (!valid(handoff)) {
        return {};
    }
    return Deadlines{contract == Contract::A ? Receipt::CommitT7 : Receipt::FinalGoalT4,
                     receive, handoff, b.receipt_to_contact_min, b.receipt_to_contact_max,
                     b.early_margin, b.late_margin};
}

Reason validate_plan(const Task& task, const PlanEnvelope& plan, Tick now,
                     Tick remaining_commit_to_receipt_upper) {
    if (task.contract != Contract::A || plan.plan_id.empty() || plan.attempt != task.attempt ||
        plan.reservation_token != task.reservation.token || plan.target != task.candidate.target ||
        !valid(plan.expected_contact)) {
        return Reason::PlanMismatch;
    }
    const auto receive_by = add_ticks(now, remaining_commit_to_receipt_upper);
    if (!receive_by || *receive_by >= plan.valid_for_receipt_until) {
        return Reason::PlanExpired;
    }
    if (*receive_by > task.deadlines.receive.close || now < task.deadlines.receive.open ||
        *receive_by > plan.expected_contact.open) {
        return Reason::ReceiveDeadline;
    }
    const auto early = add_ticks(task.contact.instants.open, task.deadlines.early_margin);
    if (!early || plan.expected_contact.open < *early) {
        return Reason::EarlyContact;
    }
    const auto late = add_ticks(plan.expected_contact.close, task.deadlines.late_margin);
    return !late || *late > task.contact.instants.close ? Reason::LateContact : Reason::None;
}

}  // namespace waste::m06
