#include "fixtures.h"
#include "test_cases.h"

using namespace waste::m08;
using namespace waste::m08::test;

void access_policy_tests() {
    Fixture f;
    const auto request = f.request("approve", Action::Approve, std::string(experimental));
    check(f.hmi.operations().submit("operator", request).issue == Issue::Unauthorized, "operator approved a package");
    check(f.hmi.operations().submit("engineer", request).issue == Issue::Unauthorized, "publisher approved its package");
    check(f.hmi.operations().submit("reviewer", request).operation->phase == Phase::Applied, "approval missing");
    check(f.hmi.operations().query("viewer", request.key).issue == Issue::NotFound, "viewer accessed other actor");
    check(f.hmi.operations().query("auditor", request.key).operation.has_value(), "audit access missing");
    const auto audits = f.hmi.operations().workbench("auditor")->audits;
    check(audits.size() == 3 && audits.front().issue == Issue::Unauthorized &&
          audits.front().request == request, "rejected request basis is not auditable");
    check(f.hmi.operations().workbench("viewer")->audits.empty(), "viewer accessed audit records");
    auto foreign = request;
    foreign.key.scope.run = "RUN-OTHER";
    check(f.hmi.operations().submit("reviewer", foreign).issue == Issue::Unauthorized, "foreign run accepted");

    const auto start = f.request("revoke", Action::Start, std::string(baseline));
    check(f.hmi.operations().submit("operator", start).operation.has_value(), "start not accepted");
    f.identity.users.erase("operator");
    f.drive();
    check(f.phase("revoke") == Phase::Failed && f.cell.submissions.empty(), "revoked actor sent device command");
}
