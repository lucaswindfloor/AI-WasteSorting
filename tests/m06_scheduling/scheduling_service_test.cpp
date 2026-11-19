#include "fake_execution.h"

#include <cstdlib>
#include <iostream>

namespace waste::m06::test {
namespace {

void revision_and_occupancy() {
    const m05::EligibilityService m05(recipe());
    SchedulingService service(config(), m05);
    FakeExecution execution;
    FakeTargets targets;
    const auto t42 = current();
    const auto t43 = current(43);
    targets.add(t42);
    targets.add(t43);
    const auto a = queued(m05, execution.data, t42, 1, 1);
    const auto b = queued(m05, execution.data, t43, 0, 0);
    const std::array batch{b, a};
    const auto sent = service.dispatch(batch, execution, targets, 50ms);
    check(sent.task && sent.task->candidate.target.id == 42 &&
          sent.task->expected_revision == 19 && sent.task->candidate.snapshot_revision == 19 &&
          sent.task->reservation == execution.data.reservations[0] && execution.requests.size() == 1 &&
          sent.task->handoff_at == 57ms && sent.task->deadlines.receive.close == 80ms &&
          sent.task->reference.count == 33815 && sent.task->applied_mm == 0 &&
          sent.task->reservation.uses[0].time.end == 202ms,
          "precheck failed after own version change or scheduled the wrong task");
    auto again = service.dispatch(batch, execution, targets, 51ms);
    check(!again.task && execution.requests.size() == 1 &&
          std::ranges::any_of(again.decisions, [](const auto& d) { return d.reason == Reason::ActiveAttempt; }) &&
          std::ranges::any_of(again.decisions, [](const auto& d) { return d.reason == Reason::NoResourceSlot; }),
          "same target or full robot occupancy not protected");
    execution.finish(sent.task->attempt, AttemptStatus::ClosedFinished);
    again = service.dispatch(batch, execution, targets, 52ms);
    check(again.task && again.task->candidate.target.id == 43,
          "already serviced target was scheduled a second time");
}

void conflict_and_changed_bin() {
    const m05::EligibilityService m05(recipe());
    {
        auto c = config(Contract::A);
        c.lanes[0].tracking = TrackingMode::ExternalReference;
        SchedulingService service(c, m05);
        FakeExecution execution;
        execution.conflict_once = true;
        FakeTargets targets;
        const auto t = current();
        targets.add(t);
        const std::array batch{queued(m05, execution.data, t)};
        const auto sent = service.dispatch(batch, execution, targets, 50ms);
        check(sent.task && execution.requests.size() == 2 && execution.requests[0].expected_revision == 18 &&
              execution.requests[1].expected_revision == 19 && sent.task->expected_revision == 20 &&
              sent.task->deadlines.receipt == Receipt::CommitT7 &&
              sent.task->reference.count == 35415 && near(sent.task->pick.x, 80) && sent.task->applied_mm == 80,
              "revision conflict or external-reference tracking did not replan");
    }
    SchedulingService service(config(), m05);
    FakeExecution execution;
    execution.bin_changes_on_reserve = true;
    FakeTargets targets;
    const auto t = current();
    targets.add(t);
    const std::array batch{queued(m05, execution.data, t)};
    auto report = service.dispatch(batch, execution, targets, 50ms);
    check(!report.task && report.decisions.back().reason == Reason::RecheckFailed &&
          execution.closes == 1 && execution.data.reservations.empty(),
          "changed bin did not close the unused old hold");
    report = service.dispatch(batch, execution, targets, 51ms);
    check(report.task && report.task->candidate.bin.identity == m05::test::backup() &&
          report.task->reservation.uses[2].resource.id == "PET-bin-2",
          "fresh M05 fallback did not acquire the replacement bin resource");
}

void elapsed_during_reservation() {
    const m05::EligibilityService m05(recipe());
    Tick time = 50ms;
    SchedulingService service(config(), m05, [&] { return time; });
    FakeExecution execution;
    execution.after_reserve = [&] { time = 58ms; };
    FakeTargets targets;
    const auto t = current();
    targets.add(t);
    const std::array batch{queued(m05, execution.data, t)};
    const auto result = service.dispatch(batch, execution, targets, 50ms);
    check(!result.task && result.decisions.back().reason == Reason::ReceiveDeadline &&
          execution.closes == 1 && execution.delivered.empty(),
          "reservation delay allowed handoff after the held slot began");
}

void uncertain_effect() {
    const m05::EligibilityService m05(recipe());
    SchedulingService service(config(), m05);
    FakeExecution execution;
    execution.uncertain_handoff = true;
    FakeTargets targets;
    const auto t = current();
    targets.add(t);
    const std::array batch{queued(m05, execution.data, t)};
    auto report = service.dispatch(batch, execution, targets, 50ms);
    check(!report.task && report.decisions.back().reason == Reason::EffectUncertain &&
          execution.delivered.size() == 1 && execution.data.reservations.size() == 1,
          "uncertain handoff lost the authoritative hold");
    const auto first = execution.delivered[0].attempt;
    // Even if a partial snapshot has lost the hold, a status query that
    // proves nothing may not be interpreted as a reset.
    execution.data.reservations.clear();
    ++execution.data.eligibility.revision;
    report = service.dispatch(batch, execution, targets, 51ms);
    check(!report.task && report.decisions.back().reason == Reason::ActiveAttempt &&
          execution.closes == 0 && execution.requests.size() == 1,
          "released or retried when effects were unknown");
    execution.uncertain_handoff = false;
    execution.finish(first, AttemptStatus::ClosedUnused);
    report = service.dispatch(batch, execution, targets, 52ms);
    check(report.task && report.task->attempt != first, "confirmed-unused attempt did not permit reassessment");
}

}  // namespace
}  // namespace waste::m06::test

int main() {
    using namespace waste::m06::test;
    try {
        contact_test();
        timing_test();
        resource_test();
        remeasurement_test();
        revision_and_occupancy();
        conflict_and_changed_bin();
        elapsed_during_reservation();
        uncertain_effect();
        std::cout << "M06: 8 scenarios passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
