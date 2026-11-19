#include "fixtures.h"

#include <cstdlib>
#include <iostream>

namespace waste::m07::test {
namespace {

void accepted_handoff_recovery() {
    Bench b;
    const auto task = b.schedule();
    check(b.store.saved->attempts.at(task.attempt).stage == Stage::Queued, "handoff not recoverable");
    b.now = 51ms;
    ExecutionService restarted(config(m06::Contract::B, "queued-restart"), b.controller, b.store,
                               b.eligibility, b.targets, [&] { return b.now; });
    check(restarted.recover() && restarted.status(task.attempt) == m06::AttemptStatus::Delivered &&
          !restarted.snapshot(), "accepted task before intent missing after restart");
    check(restarted.update_field(field(1, b.now)).issue == Issue::InvalidInput, "old field revision revived");
    check(restarted.update_field(field(2, b.now)).issue == Issue::None, "fresh field not published");
    const auto earliest = applicable_receipt(task)->open;
    b.now = earliest;
    restarted.advance();
    check(b.controller.sent.empty() &&
          b.store.saved->attempts.at(task.attempt).intents.contains(Operation::FinalGoal),
          "recovered queue skipped durable intent");
    restarted.advance();
    check(b.controller.sent.size() == 1, "recoverable queued work was not delivered");
}

}  // namespace
}  // namespace waste::m07::test

int main() {
    using namespace waste::m07::test;
    try {
        reservation_test();
        command_test();
        reducer_test();
        recovery_test();
        accepted_handoff_recovery();
        std::cout << "M07: 5 scenario groups passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
