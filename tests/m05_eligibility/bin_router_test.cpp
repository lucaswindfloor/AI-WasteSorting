#include "fixtures.h"

namespace waste::m05::test {

void bin_test() {
    const auto r = recipe();
    const EligibilityService service(r);
    const auto t = target();
    const auto g = grid(t);
    auto snapshot = facts();
    const auto initial = evaluate(service, t, snapshot, g);
    check(initial.candidates[0].bin.identity == primary() && initial.candidates[0].valid_until == 60ms &&
          service.recheck(initial.candidates[0], t, snapshot, 20ms).state == State::Ready,
          "primary route or earliest expiry is wrong");
    snapshot.revision = 19;
    snapshot.bins[0].condition = BinCondition::Full;
    snapshot.bins[0].time = {"PLC", 16ms, 28ms};
    check(service.recheck(initial.candidates[0], t, snapshot, 17ms).reason == Reason::SnapshotVersionChanged,
          "old snapshot gave an online candidate");
    const auto fallback = evaluate(service, t, snapshot, g, 17ms);
    check(fallback.candidates[0].bin.identity == backup() && fallback.candidates[0].valid_until == 28ms &&
          fallback.bins[0].check.reason == Reason::BinFull,
          "approved fallback or expiry of route prerequisite failed");
    auto changed_fact = snapshot;
    changed_fact.bins[1].placement_revision++;
    check(service.recheck(fallback.candidates[0], t, changed_fact, 18ms).reason == Reason::BinIdentityMismatch,
          "replacement physical destination reused the same placement binding");
    changed_fact = snapshot;
    changed_fact.bins[0].time.observed = 17ms;
    check(service.recheck(fallback.candidates[0], t, changed_fact, 18ms).reason == Reason::SnapshotInvalid,
          "same snapshot revision hid a change to a fallback prerequisite");
    snapshot.bins[1].condition = BinCondition::Changing;
    check(evaluate(service, t, snapshot, g, 17ms).reason == Reason::BinChanging,
          "backup was used during changeover");
    snapshot.bins[1].condition = BinCondition::Ready;
    snapshot.bins[0].identity.physical_bin = "unconfirmed";
    check(evaluate(service, t, snapshot, g, 17ms).reason == Reason::BinIdentityMismatch,
          "unconfirmed primary identity authorized fallback");
    snapshot.bins[0].identity = primary();
    snapshot.bins[0].time.valid_until = 17ms;
    check(evaluate(service, t, snapshot, g, 17ms).reason == Reason::BinStale,
          "stale full report authorized fallback");

    auto only_primary = r;
    only_primary.materials.at("PET").bins = {primary()};
    snapshot.bins[0].time.valid_until = 30ms;
    const auto full = evaluate(EligibilityService(only_primary), t, snapshot, g, 17ms);
    check(full.state == State::Rejected && full.reason == Reason::BinFull &&
          full.material_check.state == State::Ready && full.regions[0].contact.check.state == State::Ready,
          "no available destination hid valid material and geometry");
}

}  // namespace waste::m05::test
