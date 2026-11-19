#include "remeasurement_policy.h"

#include <algorithm>

namespace waste::m05 {

bool eligible_for_remeasurement(const Recipe& recipe, const MaterialRule& material, const Contact& contact) {
    if (!material.allow_remeasurement || contact.check.state != State::Pending ||
        contact.missing.empty() || contact.sources.empty()) {
        return false;
    }
    return std::ranges::all_of(contact.missing, [&](auto item) {
        return std::ranges::find(recipe.remeasurement_components, item) != recipe.remeasurement_components.end();
    }) && std::ranges::all_of(contact.reasons, [](auto item) {
        return item == Reason::SurfaceMissing || item == Reason::CenterMissing || item == Reason::NormalMissing;
    });
}

bool usable_remeasurement(const Remeasurement& request, const RemeasurementProof& proof,
                          const m04::Target& target, const Recipe& recipe, Tick now) {
    const auto current = binding(target);
    return proof.same_target_verified && !proof.appointment_id.empty() &&
           request.target == proof.requested && current == proof.observed &&
           current.generation == request.target.generation && current.id == request.target.id &&
           current.version > request.target.version && request.recipe_version == recipe.version &&
           request.profile_version == recipe.coverage.version && request.bin_mapping_version == recipe.bin_mapping_version &&
           proof.region_id == request.region_id && !proof.proof_id.empty() && proof.proof_id != request.proof_id &&
           request.assessed >= request.target.reference.time &&
           proof.measured >= request.assessed && proof.measured <= now &&
           now < request.valid_until && now < proof.valid_until;
}

}  // namespace waste::m05
