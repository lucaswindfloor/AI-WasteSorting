#include "eligibility_service.h"

#include <algorithm>
#include <set>
#include <utility>

namespace waste::m05 {
namespace {

Result start_result(const m04::Target& target, const Recipe& recipe, const Snapshot& snapshot, Tick now) {
    Result result;
    result.target = binding(target);
    result.recipe_version = recipe.version;
    result.snapshot_revision = snapshot.revision;
    result.assessed = now;
    // Protect the deterministic per-call work limit when receiving a malformed target.
    if (target.material.supporting.size() <= recipe.limits.max_points &&
        target.material.disputed.size() <= recipe.limits.max_points - target.material.supporting.size()) {
        result.material_sources = target.material.supporting;
        result.disputed_material = target.material.disputed;
    }
    return result;
}

void finish(Result& result, const Check& check) {
    result.state = check.state;
    result.reason = check.reason;
}

Reason validate_observation(const Observation& input, const Limits& limits) {
    if (input.grids.size() > limits.max_grids || input.regions.size() > limits.max_regions) {
        return Reason::Capacity;
    }
    std::size_t cells = 0;
    std::set<std::string> proofs;
    for (const auto& grid : input.grids) {
        if (grid.cells.size() > limits.max_cells - cells) {
            return Reason::Capacity;
        }
        cells += grid.cells.size();
        if (grid.proof_id.empty() || !proofs.insert(grid.proof_id).second) {
            return Reason::InvalidSurface;
        }
    }
    std::set<std::string> regions;
    for (const auto& region : input.regions) {
        if (region.grid_index >= input.grids.size() || region.id.empty() ||
            !regions.insert(region.id).second || !finite(region.x) || !finite(region.y)) {
            return Reason::InvalidSurface;
        }
    }
    return Reason::None;
}

Check first_unready(Result& result) {
    if (result.bin_route.state != State::Ready) {
        return result.bin_route;
    }
    const auto available = std::ranges::find_if(result.tools, [](const auto& tool) {
        return tool.check.state == State::Ready;
    });
    if (available == result.tools.end()) {
        const auto waiting = std::ranges::find_if(result.tools, [](const auto& tool) {
            return tool.check.state == State::Pending;
        });
        return (waiting == result.tools.end() ? result.tools.front() : *waiting).check;
    }
    const auto waiting = std::ranges::find_if(result.regions, [](const auto& region) {
        return region.contact.check.state == State::Pending;
    });
    if (waiting != result.regions.end()) {
        return waiting->contact.check;
    }
    return result.regions.empty() ? Check{State::Pending, Reason::SurfaceMissing, {}}
                                  : result.regions.front().contact.check;
}

}  // namespace

EligibilityService::EligibilityService(Recipe recipe) : recipe_(std::move(recipe)) {
    validate(recipe_);
}

Result EligibilityService::evaluate(const m04::Target& target, const Snapshot& snapshot,
                                    const Observation& input, Tick now) const {
    auto result = start_result(target, recipe_, snapshot, now);
    result.target_check = check_target(target, recipe_, now);
    if (result.target_check.state != State::Ready) {
        finish(result, result.target_check);
        return result;
    }
    if (target.material.supporting.size() > recipe_.limits.max_points ||
        target.material.disputed.size() > recipe_.limits.max_points - target.material.supporting.size()) {
        result.material_check = {State::Pending, Reason::Capacity, {}};
        finish(result, result.material_check);
        return result;
    }

    const auto material = choose_material(target, recipe_, now);
    result.material_check = material.check;
    if (!material.route) {
        finish(result, material.check);
        return result;
    }
    const auto& route = *material.route;
    result.rule_label = route.route_label;
    if (!snapshot_identity_valid(snapshot, recipe_)) {
        finish(result, {State::Pending, Reason::SnapshotInvalid, {}});
        return result;
    }
    auto routing = route_bin(route, snapshot, now, recipe_.max_fact_age);
    result.bin_route = routing.check;
    result.bins = std::move(routing.checks);
    for (const auto& expected : route.tools) {
        result.tools.push_back(check_tool(expected, snapshot, now, recipe_.max_fact_age));
    }

    if (const auto issue = validate_observation(input, recipe_.limits); issue != Reason::None) {
        finish(result, {issue == Reason::Capacity ? State::Pending : State::Rejected, issue, {}});
        return result;
    }
    std::vector<BinFact> route_dependencies;
    if (routing.selected) {
        for (std::size_t i = 0; i <= *routing.selected; ++i) {
            route_dependencies.push_back(*result.bins[i].observed);
        }
    }
    const auto index = index_surface(target, recipe_.limits.max_points);
    std::vector<ContactGrid> grids;
    grids.reserve(input.grids.size());
    for (const auto& grid : input.grids) {
        grids.push_back(prepare_grid(target, grid, recipe_, index, now));
    }
    auto remaining_work = recipe_.limits.max_contact_checks;
    for (const auto& tool : result.tools) {
        if (tool.check.state != State::Ready) {
            continue;
        }
        const auto& rule = recipe_.tools.at(tool.expected);
        for (const auto& region : input.regions) {
            const auto& grid = grids[region.grid_index];
            auto contact = evaluate_contact(grid, region, recipe_.coverage, rule, now, remaining_work);
            const auto& proof_id = input.grids[region.grid_index].proof_id;
            if (routing.selected) {
                const auto& bin = *result.bins[*routing.selected].observed;
                const auto until = std::min({result.target_check.valid_until, material.check.valid_until,
                                              tool.check.valid_until, result.bin_route.valid_until,
                                              contact.check.valid_until});
                if (contact.check.state == State::Ready) {
                    result.candidates.push_back({result.target, recipe_.version, recipe_.coverage.version,
                                                 recipe_.bin_mapping_version, region.id, proof_id,
                                                 snapshot.revision, *tool.observed, bin, route.route_label,
                                                 contact, now, until, route_dependencies});
                } else if (until > now && eligible_for_remeasurement(recipe_, route, contact)) {
                    result.remeasurements.push_back({result.target, recipe_.version, recipe_.coverage.version,
                                                     recipe_.bin_mapping_version, region.id, proof_id,
                                                     snapshot.revision, *tool.observed, bin, route.route_label,
                                                     region.x, region.y, contact.required_radius_mm,
                                                     contact.missing, now, until});
                }
            }
            result.regions.push_back({region.id, proof_id, tool.expected, std::move(contact)});
        }
    }
    if (!result.candidates.empty()) {
        finish(result, {State::Ready, Reason::None, {}});
    } else if (!result.remeasurements.empty()) {
        finish(result, {State::NeedsRemeasurement, Reason::None, {}});
    } else {
        finish(result, first_unready(result));
    }
    return result;
}

Result EligibilityService::evaluate_after_remeasurement(const Remeasurement& request,
                                                         const RemeasurementProof& proof,
                                                         const m04::Target& target,
                                                         const Snapshot& snapshot,
                                                         const Observation& input, Tick now) const {
    const auto refuse = [&] {
        auto result = start_result(target, recipe_, snapshot, now);
        finish(result, {State::Pending, Reason::RemeasurementRejected, {}});
        return result;
    };
    if (!usable_remeasurement(request, proof, target, recipe_, now) ||
        validate_observation(input, recipe_.limits) != Reason::None) {
        return refuse();
    }
    const auto region = std::ranges::find(input.regions, request.region_id, &Region::id);
    if (region == input.regions.end() || input.grids[region->grid_index].proof_id != proof.proof_id) {
        return refuse();
    }
    auto result = evaluate(target, snapshot, {input.grids, std::span(&*region, 1)}, now);
    for (auto& candidate : result.candidates) {
        candidate.valid_until = std::min({candidate.valid_until, proof.valid_until, request.valid_until});
        candidate.remeasurement = RemeasurementReference{request.target, proof.appointment_id,
                                                          proof.measured, proof.valid_until};
    }
    for (auto& pending : result.remeasurements) {
        pending.valid_until = std::min({pending.valid_until, proof.valid_until, request.valid_until});
    }
    return result;
}

Check EligibilityService::recheck(const Candidate& candidate, const m04::Target& target,
                                   const Snapshot& snapshot, Tick now) const {
    return review(candidate, target, recipe_, snapshot, now);
}

}  // namespace waste::m05
