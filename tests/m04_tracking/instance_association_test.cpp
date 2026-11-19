#include "fixtures.h"

namespace waste::m04::test {

void association_test() {
    const auto c = settings();
    const auto rgb = m01::Source::Rgb;
    const auto a = *make_footprint(c, {sample(rgb, 2, 406), sample(rgb, 3, 407)});
    const auto b = *make_footprint(c, {sample(rgb, 9, 416), sample(rgb, 10, 417)});
    const auto x = *make_footprint(c, {sample(rgb, 2, 406, 20ms, 33835, 1),
                                       sample(rgb, 3, 407, 20ms, 33835, 1)});
    Target left{1, 1, c.generation, TargetState::Tracked, Reason::None, a, {}, {}, 100ms, 10ms};
    Target right{2, 1, c.generation, TargetState::Tracked, Reason::None, b, {}, {}, 100ms, 10ms};
    const std::array<const Target*, 2> targets{&left, &right};
    std::vector<Instance> observations{{{}, x}, {{}, b}};
    auto relation = associate(c, observations, targets);
    check(relation[0].matched == 1 && relation[1].matched == 2,
          "shared source was not matched on a common count reference");
    observations[1].footprint = a;
    relation = associate(c, observations, targets);
    check(!relation[0].matched && !relation[1].matched &&
          relation[0].reason == Reason::AssociationAmbiguous && relation[1].reason == Reason::AssociationAmbiguous,
          "split was resolved into two identities");
    const auto merged = make_footprint(c, {a.points[0], b.points[0]});
    observations = {{{}, *merged}};
    relation = associate(c, observations, targets);
    check(relation[0].candidates.size() == 2 && !relation[0].matched,
          "merge was associated to one neighbor");
    observations = {{{}, *make_footprint(c, {sample(rgb, 50, 406)})}};
    relation = associate(c, observations, targets);
    check(relation[0].reason == Reason::AssociationAmbiguous && relation[0].candidates.size() == 1,
          "geometry alone silently confirmed the identity");

    auto near_a_neighbor = right;
    near_a_neighbor.footprint = *make_footprint(c, {sample(rgb, 100, 403)});
    const std::array<const Target*, 2> adjacent{&left, &near_a_neighbor};
    const auto disputed = locate_owner(c, sample(m01::Source::Hsi, 55, 404.5), adjacent);
    check(!disputed.target && disputed.plausible.size() == 2, "neighbor spectral point was assigned");
}

}  // namespace waste::m04::test
