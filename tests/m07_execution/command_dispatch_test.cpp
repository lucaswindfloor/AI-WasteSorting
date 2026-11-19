#include "fixtures.h"

namespace waste::m07::test {

void command_test() {
    Bench a(m06::Contract::A);
    const auto task = a.schedule();
    check(a.execution.handoff(task) == m06::Reply::Accepted, "identical handoff after revision rejected");
    auto changed = task;
    changed.pick.z += 1;
    check(a.execution.handoff(changed) == m06::Reply::Rejected, "different handoff for same attempt accepted");
    a.advance_at(task.handoff_at);
    check(a.controller.sent.empty(), "planning command sent before checkpoint cycle");
    a.advance_at(task.handoff_at);
    check(a.controller.sent.size() == 1 && a.controller.sent[0].command.request.operation == Operation::Prepare,
          "A did not first send planning-only PREPARE");
    a.now = task.handoff_at + 5ms;
    auto plan = controller_event(task, Operation::Prepare, EventKind::PlanReady, 1, a.now);
    plan.plan = m06::PlanEnvelope{task.attempt, task.candidate.target, "plan-1", task.reservation.token,
                                 100ms, {150ms, 164ms}};
    check(a.execution.ingest(plan).issue == Issue::None, "actual A plan rejected");
    const auto commit_at = applicable_receipt(task)->open;
    a.advance_at(commit_at, 2);
    check(a.controller.sent.size() == 2 && a.controller.sent[1].command.plan_id == "plan-1" &&
          a.controller.sent[1].command.request.operation == Operation::Commit,
          "A COMMIT not bound to prechecked plan");

    Bench b;
    const auto goal = b.schedule();
    const auto earliest = applicable_receipt(goal)->open;
    b.store.fake_ack_once = true;
    b.advance_at(earliest, 2);
    check(b.controller.sent.empty() && !b.execution.state(),
          "mismatched recoverable confirmation authorized B");
    Bench late;
    const auto late_goal = late.schedule();
    late.advance_at(applicable_receipt(late_goal)->open);
    late.advance_at(applicable_receipt(late_goal)->close);
    check(late.controller.sent.empty() &&
          late.execution.status(late_goal.attempt) == m06::AttemptStatus::ClosedUnused,
          "checkpoint delay was not counted before transmission");

    Bench gate;
    const auto blocked = gate.schedule();
    gate.advance_at(applicable_receipt(blocked)->open);
    auto changed_field = field(2, gate.now);
    changed_field.safety_mirror.permits = false;
    gate.execution.update_field(changed_field);
    gate.execution.advance();
    check(gate.controller.sent.empty() &&
          gate.execution.status(blocked.attempt) == m06::AttemptStatus::ClosedUnused,
          "new business gate was not rechecked after durable confirmation");
}

}  // namespace waste::m07::test
