#include "candidate_selector.h"

#include <algorithm>
#include <tuple>

namespace waste::m06 {

std::optional<m05::Candidate> refresh_candidate(const m05::EligibilityService& eligibility,
                                                const m05::Candidate& prior, const CurrentTarget& current,
                                                const m05::Snapshot& snapshot, Tick now, bool strict_route) {
    if (target_key(prior.target) != TargetKey{current.target.generation.run_id, current.target.id} ||
        prior.target.generation != current.target.generation ||
        (strict_route && prior.target != m05::binding(current.target))) {
        return {};
    }

    // A fresh decision uses actual evidence, including a new revision
    // returned by the reservation itself. Retain E11 proof restrictions.
    const auto same_route = [&](const m05::Candidate& candidate) {
        return candidate.tool.identity == prior.tool.identity && candidate.bin.identity == prior.bin.identity &&
               candidate.bin.placement_revision == prior.bin.placement_revision &&
               candidate.region_id == prior.region_id && candidate.rule_label == prior.rule_label &&
               candidate.recipe_version == prior.recipe_version && candidate.profile_version == prior.profile_version &&
               candidate.bin_mapping_version == prior.bin_mapping_version && candidate.proof_id == prior.proof_id &&
               candidate.contact.center == prior.contact.center;
    };
    if (prior.remeasurement && (prior.target != m05::binding(current.target) ||
                                 now >= prior.remeasurement->valid_until || now >= prior.valid_until)) {
        return {};
    }
    if (eligibility.recheck(prior, current.target, snapshot, now).state == m05::State::Ready) {
        return prior;
    }
    const auto fresh = eligibility.evaluate(current.target, snapshot, {current.grids, current.regions}, now);
    const auto available = [&](const auto& candidate) {
        return candidate.region_id == prior.region_id;
    };
    auto it = std::ranges::find_if(fresh.candidates, same_route);
    if (it == fresh.candidates.end() && !(strict_route || prior.remeasurement)) {
        it = std::ranges::find_if(fresh.candidates, [&](const auto& candidate) {
            return available(candidate) && candidate.tool.identity == prior.tool.identity;
        });
        if (it == fresh.candidates.end()) {
            it = std::ranges::find_if(fresh.candidates, available);
        }
    }
    if (it == fresh.candidates.end()) {
        return {};
    }
    auto result = *it;
    if (prior.remeasurement) {
        result.remeasurement = prior.remeasurement;
        result.valid_until = std::min(result.valid_until, prior.valid_until);
    }
    return result;
}

bool preferred(const Selection& a, const Selection& b) {
    if (a.priority != b.priority) {
        return a.priority > b.priority;
    }
    return std::tie(a.deadlines.handoff.close, a.arrival_sequence, a.candidate.target.id, a.handoff_at,
                    a.candidate.region_id, a.contact.region_id) <
           std::tie(b.deadlines.handoff.close, b.arrival_sequence, b.candidate.target.id, b.handoff_at,
                    b.candidate.region_id, b.contact.region_id);
}

SelectionResult evaluate_candidate(const SchedulingConfig& config, const QueuedCandidate& entry,
                                    const CurrentTarget& current, const m05::Candidate& refreshed,
                                    const ExecutionSnapshot& snapshot, Tick now,
                                    std::optional<Tick> fixed_handoff, std::string_view own_token) {
    if (refreshed.snapshot_revision != snapshot.eligibility.revision ||
        refreshed.target != m05::binding(current.target)) {
        return {{}, Reason::CandidateStale};
    }
    const auto* lane = find_lane(config, refreshed.tool.identity.station);
    if (!lane) {
        return {{}, Reason::ResourceUnavailable};
    }
    const auto ready = fixed_handoff ? std::optional(now) : add_ticks(now, config.transaction_lead);
    if (!ready || (fixed_handoff && *fixed_handoff < *ready)) {
        return {{}, Reason::ReceiveDeadline};
    }
    const auto offsets = required_resources(*lane, refreshed);
    SelectionResult result{{}, Reason::NoContactWindow};
    for (const auto& region : lane->contact_regions) {
        const auto contact = contact_window(config, refreshed, current.prediction, current.forecast, region, now);
        if (!contact.window) {
            if (result.reason == Reason::NoContactWindow) {
                result.reason = contact.reason;
            }
            continue;
        }
        const auto deadlines = contract_deadlines(lane->contract, lane->budget, contact.window->instants);
        if (!deadlines) {
            result.reason = Reason::ReceiveDeadline;
            continue;
        }
        const auto start = fixed_handoff ? Window{*fixed_handoff, *fixed_handoff}
                                         : Window{*ready, Tick::max()};
        const auto feasible = intersection(deadlines->handoff, start);
        if (!feasible) {
            result.reason = Reason::ReceiveDeadline;
            continue;
        }
        const auto resource = find_resource_slot(snapshot, offsets, *feasible, now, own_token);
        if (!resource.handoff) {
            result.reason = resource.reason;
            continue;
        }
        Selection option{refreshed, *contact.window, *deadlines, resource.uses,
                         *resource.handoff, current.forecast.version, lane,
                         entry.priority, entry.arrival_sequence};
        if (!result.selected || preferred(option, *result.selected)) {
            result.selected = std::move(option);
        }
    }
    if (result.selected) {
        result.reason = Reason::None;
    }
    return result;
}

}  // namespace waste::m06
