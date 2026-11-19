#include "candidate_review.h"

#include <algorithm>

namespace waste::m05 {

Check review(const Candidate& candidate, const m04::Target& target, const Recipe& recipe,
             const Snapshot& snapshot, Tick now) {
    if (candidate.target != binding(target)) {
        return {State::Pending, Reason::TargetVersionChanged, {}};
    }
    if (candidate.recipe_version != recipe.version || candidate.profile_version != recipe.coverage.version ||
        candidate.bin_mapping_version != recipe.bin_mapping_version) {
        return {State::Pending, Reason::RecipeChanged, {}};
    }
    if (candidate.snapshot_revision != snapshot.revision) {
        return {State::Pending, Reason::SnapshotVersionChanged, {}};
    }
    if (!snapshot_identity_valid(snapshot, recipe)) {
        return {State::Pending, Reason::SnapshotInvalid, {}};
    }
    if (now < candidate.assessed || candidate.contact.check.state != State::Ready ||
        !candidate.contact.center ||
        now >= candidate.valid_until || now >= candidate.contact.check.valid_until) {
        return {State::Pending, Reason::CandidateExpired, {}};
    }
    const auto target_check = check_target(target, recipe, now);
    if (target_check.state != State::Ready) {
        return target_check;
    }
    const auto material = choose_material(target, recipe, now);
    if (!material.route) {
        return material.check;
    }
    if (material.route->route_label != candidate.rule_label ||
        std::ranges::find(material.route->tools, candidate.tool.identity) == material.route->tools.end()) {
        return {State::Pending, Reason::RecipeChanged, {}};
    }
    const auto tool = check_tool(candidate.tool.identity, snapshot, now, recipe.max_fact_age);
    if (tool.check.state != State::Ready) {
        return tool.check;
    }
    if (*tool.observed != candidate.tool) {
        return {State::Pending, Reason::SnapshotInvalid, {}};
    }
    const auto bin = route_bin(*material.route, snapshot, now, recipe.max_fact_age);
    if (!bin.selected) {
        return bin.check;
    }
    const auto& selected = *bin.checks[*bin.selected].observed;
    if (selected.identity != candidate.bin.identity ||
        selected.placement_revision != candidate.bin.placement_revision) {
        return {State::Pending, Reason::BinIdentityMismatch, {}};
    }
    if (candidate.route_dependencies.size() != *bin.selected + 1 ||
        !std::equal(candidate.route_dependencies.begin(), candidate.route_dependencies.end(), bin.checks.begin(),
                    [](const BinFact& previous, const BinCheck& current) {
                        return current.observed && previous == *current.observed;
                    }) || selected != candidate.bin) {
        return {State::Pending, Reason::SnapshotInvalid, {}};
    }
    return {State::Ready, Reason::None,
            std::min({candidate.valid_until, target_check.valid_until, material.check.valid_until,
                      tool.check.valid_until, bin.check.valid_until})};
}

}  // namespace waste::m05
