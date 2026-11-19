#pragma once

#include "contact_window.h"
#include "contract_timing.h"
#include "resource_schedule.h"

namespace waste::m06 {

struct Selection {
    m05::Candidate candidate;
    ContactWindow contact;
    Deadlines deadlines;
    std::vector<ResourceUse> uses;
    Tick handoff_at{};
    std::string forecast_version;
    const Lane* lane{};
    int priority{};
    std::uint64_t arrival_sequence{};
};

struct SelectionResult {
    std::optional<Selection> selected;
    Reason reason{Reason::None};
};

std::optional<m05::Candidate> refresh_candidate(const m05::EligibilityService& eligibility,
                                                const m05::Candidate& prior, const CurrentTarget& current,
                                                const m05::Snapshot& snapshot, Tick now, bool strict_route);
SelectionResult evaluate_candidate(const SchedulingConfig& config, const QueuedCandidate& entry,
                                    const CurrentTarget& current, const m05::Candidate& refreshed,
                                    const ExecutionSnapshot& snapshot, Tick now,
                                    std::optional<Tick> fixed_handoff = {},
                                    std::string_view own_token = {});
bool preferred(const Selection& a, const Selection& b);

}  // namespace waste::m06
