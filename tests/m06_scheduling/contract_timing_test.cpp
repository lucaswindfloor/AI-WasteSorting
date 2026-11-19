#include "fixtures.h"

namespace waste::m06::test {

void timing_test() {
    const Window window{135ms, 165ms};
    const auto a = config(Contract::A).lanes[0].budget;
    const auto b = config(Contract::B).lanes[0].budget;
    const auto commit = contract_deadlines(Contract::A, a, window);
    const auto goal = contract_deadlines(Contract::B, b, window);
    check(commit && goal && commit->receipt == Receipt::CommitT7 && goal->receipt == Receipt::FinalGoalT4 &&
          commit->receive.close == 100ms && goal->receive.close == 70ms &&
          commit->handoff.close == 80ms && goal->handoff.close == 60ms &&
          commit->receive.open == 79ms, "A/B receipt, handoff or opening boundary wrong");
    const auto expires = contract_deadlines(Contract::A, a, window, 95ms);
    check(expires && expires->receive.close == 95ms - 1ns, "plan expiry not narrowed");
    auto too_uncertain = b;
    too_uncertain.receipt_to_contact_min = 0ms;
    check(!contract_deadlines(Contract::B, too_uncertain, window),
          "window closed on the late side but too early on the other");

    Task task{};
    task.contract = Contract::A;
    task.attempt = {{"P0", 42}, "session", 1};
    task.reservation.token = "hold-A";
    task.candidate.target = m05::binding(current().target);
    task.contact.instants = window;
    task.deadlines = *commit;
    PlanEnvelope plan{task.attempt, task.candidate.target, "plan-A", "hold-A", 98ms, {140ms, 155ms}};
    check(validate_plan(task, plan, 90ms, 5ms) == Reason::None, "valid returned plan rejected");
    check(validate_plan(task, plan, 94ms, 5ms) == Reason::PlanExpired, "old plan to commit admitted");
    plan.expected_contact.open = 130ms;
    check(validate_plan(task, plan, 90ms, 5ms) == Reason::EarlyContact, "early plan admitted");
    plan.expected_contact = {140ms, 163ms};
    check(validate_plan(task, plan, 90ms, 5ms) == Reason::LateContact, "late contact margin ignored");
    plan.reservation_token = "other";
    check(validate_plan(task, plan, 90ms, 5ms) == Reason::PlanMismatch, "different reservation accepted");
}

}  // namespace waste::m06::test
