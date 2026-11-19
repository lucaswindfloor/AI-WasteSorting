#include "fixtures.h"

namespace waste::m05::test {

void rules_test() {
    const EligibilityService service(recipe());
    auto t = target();
    const auto g = grid(t);
    auto snapshot = facts();
    snapshot.tools[0].identity.tcp = "old-tcp";
    check(evaluate(service, t, snapshot, g).reason == Reason::ToolMismatch,
          "physical tool did not match configured TCP");
    snapshot = facts();
    snapshot.tools[0].time.valid_until = 15ms;
    check(evaluate(service, t, snapshot, g).reason == Reason::ToolStale,
          "expired tool fact accepted");
    snapshot = facts();
    auto wrong = t;
    wrong.material.status = m04::MaterialStatus::Conflict;
    check(evaluate(service, wrong, snapshot, g).reason == Reason::MaterialConflict,
          "conflicting material not distinguished");
    wrong = t;
    wrong.material.label = "PP";
    check(evaluate(service, wrong, snapshot, g).reason == Reason::MaterialExcluded,
          "known material outside recipe accepted");
    wrong = t;
    wrong.material.valid_until = 15ms;
    check(evaluate(service, wrong, snapshot, g).reason == Reason::MaterialExpired,
          "expired classification accepted");
    wrong = t;
    wrong.state = m04::TargetState::Ambiguous;
    check(evaluate(service, wrong, snapshot, g).reason == Reason::AmbiguousTarget,
          "ambiguous tracked identity accepted");

    auto policy = recipe();
    policy.unknown = UnknownRule{{m04::MaterialStatus::Insufficient},
                                 {"approved-reject-bin", {cup()}, {backup()}, {}, false}};
    const EligibilityService restricted(policy);
    t.material = {};
    const auto special = evaluate(restricted, t, snapshot, g);
    check(special.state == State::Ready && special.candidates.front().rule_label == "approved-reject-bin" &&
          special.candidates.front().bin.identity == backup(),
          "explicit policy for unknown material went to a material bin");
}

}  // namespace waste::m05::test
