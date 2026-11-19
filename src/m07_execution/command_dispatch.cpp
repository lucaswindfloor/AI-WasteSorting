#include "command_dispatch.h"
#include "m06_scheduling/contract_timing.h"

namespace waste::m07 {

std::optional<Operation> next_operation(const AttemptRecord& r) {
    if (!r.task || closed(r.stage)) {
        return {};
    }
    if (r.cancellation_requested && has_live_effect(r) && !r.intents.contains(Operation::Cancel)) {
        return Operation::Cancel;
    }
    if (r.cancellation_requested || r.needs_query) {
        return {};
    }
    if (r.stage == Stage::Queued) {
        return r.task->contract == m06::Contract::A ? Operation::Prepare : Operation::FinalGoal;
    }
    if (r.stage == Stage::PlanReady) {
        return Operation::Commit;
    }
    return {};
}

Issue send_gate(const ExecutionConfig& c, const DurableState& state,
                const AttemptRecord& r, Operation operation, Tick now) {
    if (!r.task || !r.controller || closed(r.stage)) {
        return Issue::BadIdentity;
    }
    const auto& task = *r.task;
    const auto* lane = lane_for(c, task.candidate.tool.identity.station);
    if (!lane) {
        return Issue::BadIdentity;
    }
    const auto* link = state.field ? current_link(state.field->controllers, lane->controller, now,
                                                c.max_field_age) : nullptr;
    if (!link || ControllerIdentity{link->device, link->session} != *r.controller) {
        return Issue::FieldBlocked;
    }
    if (operation == Operation::Cancel) {
        return Issue::None;
    }
    if (r.cancellation_requested || r.needs_query) {
        return Issue::FieldBlocked;
    }
    if (const auto issue = check_task(c, state, r, task, now); issue != Issue::None) {
        return issue;
    }
    if (now < task.handoff_at) {
        return Issue::NotReady;
    }
    const auto window = applicable_receipt(task);
    const auto latest_receipt = m06::add_ticks(now, lane->send_to_receipt_upper);
    if (!window || !latest_receipt || *latest_receipt > window->close) {
        return Issue::Expired;
    }
    if (operation == Operation::Prepare) {
        return lane->prepare_is_planning_only ? Issue::None : Issue::InvalidInput;
    }
    if (now < window->open) {
        // The lower transport bound is zero in the portable adapter.
        return Issue::NotReady;
    }
    if (operation == Operation::Commit) {
        if (!r.plan) {
            return Issue::PlanInvalid;
        }
        auto bounded = task;
        bounded.deadlines.receive = *window;
        const auto fit = m06::validate_plan(bounded, *r.plan, now, lane->send_to_receipt_upper);
        if (fit != m06::Reason::None) {
            return fit == m06::Reason::PlanExpired ? Issue::Expired : Issue::PlanInvalid;
        }
    }
    return Issue::None;
}

void apply_transport(AttemptRecord& r, Operation operation, Transport result, Tick now) {
    auto& intent = r.intents.at(operation);
    if (result == Transport::DefinitelyNotSent) {
        intent.progress = Progress::NotSent;
        r.issue = Issue::ControllerRejected;
        if (has_live_effect(r)) {
            r.stage = Stage::Pending;
            r.needs_query = true;
        } else {
            r.stage = Stage::ClosedUnused;
        }
        return;
    }
    if (result == Transport::Submitted) {
        intent.progress = Progress::Sent;
        intent.sent_at = now;
        if (operation == Operation::Cancel) {
            r.stage = Stage::Pending;
            r.needs_query = true;
        }
        return;
    }
    intent.progress = Progress::Uncertain;
    r.stage = Stage::Pending;
    r.needs_query = true;
    r.issue = Issue::TransportUncertain;
}

}  // namespace waste::m07
