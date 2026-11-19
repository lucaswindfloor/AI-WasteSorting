#include "fixtures.h"
#include "test_cases.h"

using namespace waste::m08;
using namespace waste::m08::test;

void bin_change_tests() {
    Fixture f;
    auto& bin = f.cell.runtime.bins.front();
    bin.no_unresolved = false;
    const auto request = f.request("replace", Action::ChangeBin, "B01", "PET-02");
    check(f.hmi.operations().submit("operator", request).operation.has_value(), "change request rejected");
    f.drive(3);
    check(f.phase("replace") == Phase::WaitingClear && !bin.usable, "held object did not block change");
    bin.no_unresolved = true;
    f.cell.runtime.safety.permits = false;
    f.drive();
    check(f.phase("replace") == Phase::WaitingClear, "unsafe mirror did not block change");
    f.cell.runtime.safety.permits = true;
    f.drive();
    check(f.phase("replace") == Phase::CanChange, "safe change stage missing");
    f.cell.replace("OTHER");
    f.drive();
    check(f.phase("replace") == Phase::CanChange && !bin.usable, "wrong box was mapped");
    f.cell.replace("PET-02");
    f.drive(2);
    check(!bin.usable && bin.mapping == 2, "mapping alone released box");
    f.drive(3);
    check(f.phase("replace") == Phase::Applied && bin.usable && bin.fence.empty(), "new box not released");
}
