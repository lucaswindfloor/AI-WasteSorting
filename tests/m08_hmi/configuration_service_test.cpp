#include "fixtures.h"
#include "test_cases.h"

using namespace waste::m08;
using namespace waste::m08::test;

void configuration_tests() {
    Fixture f;
    const auto candidate = [&](std::string id, Action action, std::string_view package) {
        return f.request(std::move(id), action, std::string(package));
    };
    const auto unapproved = candidate("not-reviewed", Action::Publish, experimental);
    check(f.hmi.operations().submit("engineer", unapproved).issue == Issue::Conditions,
          "unapproved configuration published");
    const auto approve = [&](std::string_view package) {
        check(f.hmi.operations().submit("reviewer", candidate("approve-" + std::string(package),
              Action::Approve, package)).operation.has_value(), "approval failed");
    };
    approve(experimental);
    approve(broken);
    check(f.hmi.operations().submit("engineer", candidate("publish-e11", Action::Publish, experimental))
          .operation.has_value(), "E11 publish rejected");
    f.drive(10);
    check(f.phase("publish-e11") == Phase::Applied && f.cell.runtime.epoch == 2 &&
          f.cell.runtime.active->package == experimental && f.cell.runtime.line.fence.empty(),
          "E11 never activated or cleared fence");

    check(f.hmi.operations().submit("engineer", candidate("publish-bad", Action::Publish, broken))
          .operation.has_value(), "bad package could not reach compatibility check");
    f.drive(6);
    check(f.phase("publish-bad") == Phase::Failed && f.cell.runtime.epoch == 2 &&
          f.cell.runtime.staged->problems.contains("M03") && !f.cell.runtime.line.fence.empty(),
          "partial load did not preserve actual configuration and fence");

    check(f.hmi.operations().submit("engineer", candidate("restore", Action::Rollback, baseline))
          .operation.has_value(), "prior activated baseline not eligible for rollback");
    f.drive(10);
    check(f.phase("restore") == Phase::Applied && f.cell.runtime.epoch == 4 &&
          f.journal.saved->activated.contains(2) && !f.journal.saved->activated.contains(3) &&
          f.cell.runtime.active->package == baseline && f.cell.runtime.line.mode == LineMode::Paused,
          "rollback failed to record new epoch without restarting production");
}
