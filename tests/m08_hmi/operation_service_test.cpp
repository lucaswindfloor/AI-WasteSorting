#include "fixtures.h"
#include "test_cases.h"

using namespace waste::m08;
using namespace waste::m08::test;

void operation_service_tests() {
    Fixture f;
    auto old = f.request("old-ui", Action::Start, std::string(baseline));
    f.cell.changed();
    check(f.hmi.operations().submit("operator", old).issue == Issue::StaleView, "stale version accepted");
    const auto start = f.request("start", Action::Start, std::string(baseline));
    check(f.hmi.operations().submit("operator", start).operation->phase == Phase::Accepted, "missing acceptance");
    check(f.hmi.operations().submit("operator", start).operation.has_value(), "idempotent lookup failed");
    auto conflict = start;
    conflict.value = "different";
    check(f.hmi.operations().submit("operator", conflict).issue == Issue::Conflict, "changed retry did not conflict");

    f.cell.unknown_delivery = true;
    f.cell.effects_enabled = false;
    f.drive(6);
    check(f.phase("start") == Phase::NeedsReview && f.cell.runtime.line.mode == LineMode::Idle,
          "delivery was treated as actual motion");
    HmiService recovered(settings(), f.identity, f.cell, f.journal, f.archive, [&] { return f.now; });
    check(recovered.recover(), "journal recovery failed");
    f.cell.effects_enabled = true;
    recovered.advance();
    recovered.advance();
    check(recovered.operations().query("operator", start.key).operation->phase == Phase::Applied &&
          f.cell.submissions.size() == 1, "recovery failed to resolve original command without redispatch");

    const auto drain = f.request("drain", Action::Drain);
    check(recovered.operations().submit("operator", drain).operation.has_value(), "drain not accepted");
    for (int i = 0; i < 3; ++i) recovered.advance();
    check(recovered.operations().query("operator", drain.key).operation->phase == Phase::Applied &&
          f.cell.runtime.line.mode == LineMode::Ended, "drain completion missing");

    Fixture g;
    const auto uncommitted = g.request("bad-commit", Action::Start, std::string(baseline));
    check(g.hmi.operations().submit("operator", uncommitted).operation.has_value(), "setup failed");
    g.journal.bad_confirmation = true;
    g.drive();
    check(g.cell.submissions.empty(), "device received unconfirmed durable command");
    g.journal.bad_confirmation = false;
    check(g.hmi.recover(), "failed to reload ambiguous checkpoint");
    g.drive(5);
    check(g.phase("bad-commit") == Phase::NeedsReview && g.cell.submissions.empty(),
          "ambiguous checkpoint must be reconciled by original ID");
}
