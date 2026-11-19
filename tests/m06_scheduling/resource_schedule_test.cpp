#include "fixtures.h"

namespace waste::m06::test {

void resource_test() {
    const auto c = config();
    const auto t = current();
    const m05::EligibilityService m05(recipe());
    auto data = snapshot();
    const auto entry = queued(m05, data, t);
    const auto required = required_resources(c.lanes.front(), entry.candidate);
    data.calendars[0].occupied = {{0ms, 60ms}};
    auto slot = find_resource_slot(data, required, {57ms, 70ms}, 50ms);
    check(slot.handoff == 60ms && slot.uses[0].time == Interval{60ms, 205ms} &&
          slot.uses[2].time == Interval{172ms, 185ms}, "full occupancy or half-open boundary wrong");
    data.calendars[2].usable = {{179ms, 220ms}};
    slot = find_resource_slot(data, required, {57ms, 70ms}, 50ms);
    check(slot.handoff == 67ms, "bin availability was checked at grab time instead of drop period");
    data.calendars[0].occupied.push_back({210ms, 300ms});
    check(find_resource_slot(data, required, {57ms, 70ms}, 50ms).reason == Reason::NoResourceSlot,
          "return interval conflict ignored");
    data = snapshot();
    const auto original = find_resource_slot(data, required, {57ms, 70ms}, 50ms);
    data.reservations.push_back({{{"P0", 42}, "test", 1}, "held", original.uses, 71ms});
    check(!find_resource_slot(data, required, {57ms, 70ms}, 50ms).handoff &&
          find_resource_slot(data, required, {57ms, 57ms}, 50ms, "held").uses == original.uses,
          "existing hold ignored or own hold blocked reuse");
    data.calendars[0].valid_until = 50ms;
    check(find_resource_slot(data, required, {57ms, 70ms}, 50ms).reason == Reason::ResourceUnavailable,
          "expired field calendar accepted");
}

}  // namespace waste::m06::test
