#pragma once

#include "attempt_store.h"
#include "candidate_selector.h"

namespace waste::m06 {

using Clock = std::function<Tick()>;

class SchedulingService {
public:
    SchedulingService(SchedulingConfig config, const m05::EligibilityService& eligibility,
                      Clock clock = {});
    // Evaluate one bounded batch, hand off at most one attempt, and return
    // decisions that the caller can associate with its retained candidates.
    DispatchReport dispatch(std::span<const QueuedCandidate> batch, ExecutionPort& execution,
                            TargetSource& targets, Tick now);

private:
    Tick update_time(Tick supplied);
    Reason close_unused(ExecutionPort& execution, Attempt& attempt, Reason original);
    Reason reserve_and_handoff(const Selection& selection, const ExecutionSnapshot& snapshot,
                               ExecutionPort& execution, TargetSource& targets,
                               Decision& decision, DispatchReport& report);

    SchedulingConfig config_;
    const m05::EligibilityService& eligibility_;
    Clock clock_;
    AttemptStore attempts_;
    Tick now_{};
};

}  // namespace waste::m06
