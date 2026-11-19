#include "fixtures.h"

namespace waste::m07::test {

void recovery_test() {
    Bench b;
    const auto task = b.schedule();
    const auto earliest = applicable_receipt(task)->open;
    b.advance_at(earliest);
    b.controller.lose_send_checkpoint_once = true;
    b.advance_at(earliest);
    check(b.controller.sent.size() == 1 && !b.execution.state() &&
          b.store.saved->attempts.at(task.attempt).intents.at(Operation::FinalGoal).progress == Progress::Armed,
          "ambiguous crash fixture did not retain the pre-send checkpoint");

    b.now = earliest + 5ms;
    ExecutionService restarted(config(m06::Contract::B, "exec-restart"), b.controller, b.store,
                               b.eligibility, b.targets, [&] { return b.now; });
    check(restarted.recover() && !restarted.snapshot() &&
          restarted.status(task.attempt) == m06::AttemptStatus::Unknown, "old live facts survived restart");
    auto reconnected = field(2, b.now);
    reconnected.controllers[0].session = "session-2";
    restarted.update_field(reconnected);
    restarted.advance();
    check(b.controller.sent.size() == 1 && b.controller.queries.size() == 1 &&
          b.controller.queries[0].first.session == "session-1" &&
          b.controller.queries[0].second == RequestId{task.attempt, Operation::FinalGoal} &&
          !restarted.snapshot()->reservations.empty(), "old request resent or missing released occupancy");
    b.now += 10ms;
    b.controller.query_state = QueryState::Acting;
    b.controller.query_proof = Stamp{Source::Controller, "E01", "session-1", 7, b.now,
                                     b.now + 50ms, "original-session proof"};
    restarted.advance();
    check(restarted.status(task.attempt) == m06::AttemptStatus::Active &&
          b.controller.sent.size() == 1, "authoritative old-session query did not recover action");

    Bench stale_plan(m06::Contract::A);
    const auto old = stale_plan.schedule();
    stale_plan.advance_at(old.handoff_at, 2);
    stale_plan.now = old.handoff_at + 4ms;
    auto plan = controller_event(old, Operation::Prepare, EventKind::PlanReady, 1, stale_plan.now);
    plan.plan = m06::PlanEnvelope{old.attempt, old.candidate.target, "expired-before-send",
                                 old.reservation.token, 110ms, {150ms, 164ms}};
    stale_plan.execution.ingest(plan);
    stale_plan.advance_at(applicable_receipt(old)->open);
    stale_plan.advance_at(applicable_receipt(old)->close);
    check(stale_plan.controller.sent.size() == 1 &&
          stale_plan.execution.state()->attempts.at(old.attempt).intents.at(Operation::Commit).progress ==
              Progress::NotSent, "missed A commit reached controller");
    stale_plan.now += 1ms;
    stale_plan.controller.query_state = QueryState::TerminatedWithoutMotion;
    stale_plan.controller.query_proof = Stamp{Source::Controller, "E01", "session-1", 2, stale_plan.now,
                                              stale_plan.now + 50ms, "prepared plan removed"};
    stale_plan.execution.advance();
    stale_plan.now += 1ms;
    stale_plan.execution.ingest(controller_event(old, Operation::Prepare, EventKind::RobotReady, 3,
                                                  stale_plan.now));
    check(stale_plan.execution.status(old.attempt) == m06::AttemptStatus::ClosedUnused,
          "dead prepared plan with definitely unsent COMMIT could not be closed");

    Bench cancel;
    const auto goal = cancel.schedule();
    cancel.advance_at(applicable_receipt(goal)->open, 2);
    cancel.now = 80ms;
    check(cancel.execution.request_cancel(goal.attempt).issue == Issue::None, "cancel could not be requested");
    cancel.execution.advance();
    cancel.execution.advance();
    check(cancel.controller.sent.size() == 2 &&
          cancel.controller.sent.back().command.related_request == RequestId{goal.attempt, Operation::FinalGoal},
          "cancel did not name original operation");
    cancel.now = 81ms;
    cancel.execution.ingest(controller_event(goal, Operation::Cancel, EventKind::CancelAcknowledged, 2, 81ms));
    check(cancel.execution.status(goal.attempt) == m06::AttemptStatus::Unknown &&
          !cancel.execution.snapshot()->reservations.empty(), "cancel acknowledgement released resources");
    cancel.now = 82ms;
    cancel.execution.ingest(controller_event(goal, Operation::FinalGoal, EventKind::NoAction, 3, 82ms));
    cancel.now = 83ms;
    cancel.execution.ingest(controller_event(goal, Operation::FinalGoal, EventKind::RobotReady, 4, 83ms));
    cancel.execution.ingest(tool_event(goal, EventKind::ToolEmpty, 1, 83ms));
    cancel.execution.ingest(tool_event(goal, EventKind::ToolHealthy, 2, 83ms));
    check(cancel.execution.status(goal.attempt) == m06::AttemptStatus::ClosedUnused,
          "authoritative termination and readiness did not close unused attempt");
}

}  // namespace waste::m07::test
