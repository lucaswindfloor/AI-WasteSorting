#include "fixtures.h"

namespace waste::m07::test {

void reducer_test() {
    Bench b;
    const auto task = b.schedule();
    const auto send = applicable_receipt(task)->open;
    b.advance_at(send, 2);
    check(b.controller.sent.size() == 1, "B final target not sent");
    const auto op = Operation::FinalGoal;
    const auto feed = [&](EventKind kind, std::uint64_t seq, Tick occurred, Tick now) {
        b.now = now;
        return b.execution.ingest(controller_event(task, op, kind, seq, occurred));
    };
    check(feed(EventKind::Approaching, 3, 145ms, 145ms).issue == Issue::None, "motion event rejected");
    check(feed(EventKind::Receipt, 1, send + 1ms, 146ms).issue == Issue::None &&
          b.execution.status(task.attempt) == m06::AttemptStatus::Active,
          "late receipt moved active task backwards");
    b.now = 150ms;
    const auto holding = b.execution.ingest(tool_event(task, EventKind::Holding, 1, 150ms));
    check(holding.issue == Issue::None, "valid terminal holding event rejected");
    b.now = 151ms;
    check(b.execution.ingest(controller_event(task, op, EventKind::RobotReady, 99, 151ms, "stale-session"))
              .issue == Issue::BadIdentity, "old-session event applied");
    const auto placed = feed(EventKind::PlacementEnded, 5, 170ms, 170ms);
    check(placed.issue == Issue::None, "valid placement event rejected");
    check(b.execution.state()->attempts.at(task.attempt).carrier == Carrier::Held &&
          !b.execution.snapshot()->reservations.empty(), "placement motion falsely inferred release");
    b.now = 175ms;
    b.execution.ingest(tool_event(task, EventKind::Released, 2, 175ms));
    feed(EventKind::Returning, 6, 180ms, 180ms);
    feed(EventKind::Finished, 7, 190ms, 190ms);
    b.now = 191ms;
    b.execution.ingest(tool_event(task, EventKind::ToolEmpty, 3, 178ms));
    b.execution.ingest(tool_event(task, EventKind::ToolHealthy, 4, 191ms));
    feed(EventKind::RobotReady, 8, 192ms, 192ms);
    check(!b.execution.snapshot()->reservations.empty(), "old empty evidence released completed task");
    b.now = 193ms;
    b.execution.ingest(tool_event(task, EventKind::ToolEmpty, 5, 193ms));
    const auto current = *b.execution.snapshot();
    check(current.reservations.empty() &&
          m06::target_in_use(current, task.attempt.target) &&
          b.execution.status(task.attempt) == m06::AttemptStatus::ClosedFinished,
          "fresh readiness did not release resources, or allowed repeat target");
}

}  // namespace waste::m07::test
