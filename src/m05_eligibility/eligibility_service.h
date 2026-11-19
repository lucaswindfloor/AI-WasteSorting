#pragma once

#include "candidate_review.h"
#include "contact_geometry.h"
#include "remeasurement_policy.h"

namespace waste::m05 {

class EligibilityService {
public:
    explicit EligibilityService(Recipe recipe);
    Result evaluate(const m04::Target& target, const Snapshot& snapshot,
                    const Observation& observation, Tick now) const;
    Result evaluate_after_remeasurement(const Remeasurement& request, const RemeasurementProof& proof,
                                       const m04::Target& target, const Snapshot& snapshot,
                                       const Observation& observation, Tick now) const;
    Check recheck(const Candidate& candidate, const m04::Target& target,
                  const Snapshot& snapshot, Tick now) const;

private:
    Recipe recipe_;
};

}  // namespace waste::m05
