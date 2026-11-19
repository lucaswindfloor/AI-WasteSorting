#include "fixtures.h"

namespace waste::m07::test {

void reservation_test() {
    Bench b;
    auto snapshot = *b.execution.snapshot();
    const m06::ReservationRequest request{{{"P0", 42}, "sched", 1},
                                          m05::binding(m06::test::current().target), snapshot.eligibility.revision,
                                          {{{m06::ResourceKind::Robot, "R1"}, {57ms, 200ms}},
                                           {{m06::ResourceKind::Tool, "cup-1"}, {57ms, 190ms}},
                                           {{m06::ResourceKind::Bin, "PET-bin-1"}, {170ms, 185ms}}},
                                          57ms, 70ms};
    auto unavailable = request;
    unavailable.uses[2].resource.id = "unavailable-bin";
    check(b.execution.reserve(unavailable).status == m06::Reply::Conflict &&
          b.execution.snapshot()->reservations.empty(), "failed request reserved part of the group");
    const auto reserved = b.execution.reserve(request);
    check(reserved.status == m06::Reply::Accepted && reserved.reservation && b.store.saved->attempts.size() == 1,
          "accepted reservation not persisted");
    auto replay = request;
    check(b.execution.reserve(replay).reservation == reserved.reservation, "identical reserve not idempotent");
    replay.uses[0].time.end += 1ms;
    check(b.execution.reserve(replay).status == m06::Reply::Rejected, "different content reused a key");
    auto second = request;
    second.attempt.target.id = second.target.id = 43;
    check(b.execution.reserve(second).status == m06::Reply::Conflict, "old revision accepted");
    second.expected_revision = b.execution.snapshot()->eligibility.revision;
    check(b.execution.reserve(second).status == m06::Reply::Conflict, "overlapping physical resource accepted");
    check(b.execution.close_unused(request.attempt, "other") == m06::Reply::Uncertain &&
          b.execution.close_unused(request.attempt, reserved.reservation->token) == m06::Reply::Accepted &&
          b.execution.snapshot()->reservations.empty(), "unused reservation release failed");
}

}  // namespace waste::m07::test
