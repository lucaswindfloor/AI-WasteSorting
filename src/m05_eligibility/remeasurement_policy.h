#pragma once

#include "recipe_rules.h"

namespace waste::m05 {

struct RemeasurementProof {
    TargetBinding requested, observed;
    std::string region_id, proof_id, appointment_id;
    Tick measured{}, valid_until{};
    bool same_target_verified{};
};

bool eligible_for_remeasurement(const Recipe& recipe, const MaterialRule& material, const Contact& contact);
bool usable_remeasurement(const Remeasurement& request, const RemeasurementProof& proof,
                          const m04::Target& target, const Recipe& recipe, Tick now);

}  // namespace waste::m05
