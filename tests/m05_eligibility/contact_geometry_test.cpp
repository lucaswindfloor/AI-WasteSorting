#include "fixtures.h"

namespace waste::m05::test {

void geometry_test() {
    const EligibilityService service(recipe());
    auto t = target();
    const auto snapshot = facts();
    auto g = grid(t);
    auto result = evaluate(service, t, snapshot, g);
    check(result.state == State::Ready && result.candidates.size() == 1 &&
          near(result.candidates[0].contact.center->z, 48.6) &&
          near(result.candidates[0].contact.required_radius_mm, 1.4) &&
          result.candidates[0].contact.sources.size() == 9,
          "verified whole-circle contact was not accepted");

    g.cells[13] = {CellState::Missing};
    result = evaluate(service, t, snapshot, g);
    check(result.candidates.empty() && result.state == State::Pending &&
          result.reason == Reason::SurfaceMissing && result.regions[0].contact.center &&
          result.regions[0].contact.sources.size() == 8,
          "valid center hid a hole across the cup");
    g.cells[13] = {CellState::Neighbor};
    result = evaluate(service, t, snapshot, g);
    check(result.state == State::Rejected && result.reason == Reason::NeighborOverlap,
          "neighbor area was used as the sealing surface");
    g = grid(t);
    const std::array outside{Region{"edge", 1.8, 406, 0}};
    result = service.evaluate(t, snapshot, {std::span(&g, 1), outside}, 15ms);
    check(result.state == State::Rejected && result.reason == Reason::ContactOutside,
          "cup centered inside exceeded the measured boundary");

    g.cells[12].tilt_upper_deg = 14;
    result = evaluate(service, t, snapshot, g);
    check(result.state == State::Rejected && result.reason == Reason::TiltExceeded,
          "normal bound of contact center was ignored");
    g = grid(t);
    t.surfaces[2].points[2].position.z += 2;
    result = evaluate(service, t, snapshot, g);
    check(result.state == State::Rejected && result.reason == Reason::HeightVariation,
          "height variation across contact was missed");
    t = target();
    g = grid(t);
    g.cells[13].source = g.cells[12].source;
    check(evaluate(service, t, snapshot, g).reason == Reason::InvalidSurface,
          "one observation was allowed to certify multiple cells");

    result = service.evaluate(t, snapshot, {}, 15ms);
    check(result.state == State::Pending && result.reason == Reason::SurfaceMissing && result.candidates.empty(),
          "M04 points alone were used as whole-area coverage");
    auto constrained = recipe();
    constrained.limits.max_contact_checks = 1;
    const auto fresh = grid(t);
    check(evaluate(EligibilityService(constrained), t, snapshot, fresh).reason == Reason::Capacity,
          "contact scan escaped the configured work limit");
}

}  // namespace waste::m05::test
