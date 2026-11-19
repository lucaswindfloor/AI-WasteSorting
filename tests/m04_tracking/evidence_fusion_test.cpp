#include "fixtures.h"
#include "m04_tracking/material_fusion.h"
#include "m04_tracking/surface_fusion.h"

namespace waste::m04::test {

void evidence_test() {
    auto c = settings();
    c.min_material_samples = 2;
    EvidenceIndex index(c.limits);
    const auto site = sample(m01::Source::Hsi, 260, 406);
    const Origin w17{{c.models[0].identity.key, 2112, 2175}, 0};
    const Origin w18{{c.models[0].identity.key, 2144, 2207}, 0};
    check(index.add_material(site, {w17, "PET", 0.9f}) == Reason::None, "first material rejected");
    auto other_ref = site;
    other_ref.source.segment = 999;
    check(index.add_material(other_ref, {w18, "PET", 0.95f}) == Reason::None &&
          index.add_material(site, {w18, "PET", 0.95f}) == Reason::Duplicate &&
          index.material().size() == 1 && index.material().begin()->second.readings.size() == 2,
          "overlap counted as independent raw samples");
    std::vector<const SpectralPoint*> points{&index.material().begin()->second};
    check(fuse_material(c, points, {}).status == MaterialStatus::Insufficient,
          "two windows incorrectly satisfied unique spectral coverage");
    const Origin w19{{c.models[0].identity.key, 2142, 2205}, 0};
    index.add_material(site, {w19, "PP", 0.9f});
    check(fuse_material(c, points, {}).status == MaterialStatus::Conflict,
          "different labels on one physical sample were masked");

    auto gap_a = sample(m01::Source::E05, 0, 406);
    auto gap_b = sample(m01::Source::E05, 2, 407);
    gap_a.position.z = gap_b.position.z = 48.6;
    const std::array<const Sample*, 2> surface{&gap_a, &gap_b};
    auto patches = fuse_surface(c, ref(), surface);
    check(patches.size() == 2 && patches[0].points.size() == 1 && patches[1].points.size() == 1,
          "surface spanned a missing E05 column");
    index.expire(300ms, c.max_evidence_age);
    check(index.material().empty(), "stale observations retained");
}

}  // namespace waste::m04::test
