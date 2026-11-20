#include "fixtures.h"
#include "m09_archive/storage/retention_backup.h"

namespace waste::m09::test {

void storage_pressure() {
    const auto before = assess_pressure({1000, 100, 150, 100, 300, 500});
    check(before.usable_disk == 900 && before.required_disk == 550 && !before.limit_input, "normal operation shaded");
    const auto near_full = assess_pressure({620, 100, 150, 100, 300, 500});
    check(near_full.limit_input && near_full.limit_raw, "stop tail and command reserve omitted");
    const auto memory = assess_pressure({1000, 100, 150, 100, 300, 390});
    check(memory.limit_input && !memory.limit_raw, "in-flight bytes were not included in buffer risk");
    check(assess_pressure({UINT64_MAX, 10, UINT64_MAX, 10, 255, UINT64_MAX}).limit_input,
          "overflow bypassed pressure limit");
    Manifest sealed;
    sealed.sealed = true;
    sealed.reconciliation_pending = false;
    check(!eligible_for_retention_cleanup(sealed, {true, false, false, true}) &&
          eligible_for_retention_cleanup(sealed, {false, false, false, true}),
          "retention decision did not distinguish unresolved commands");
}

}  // namespace waste::m09::test
