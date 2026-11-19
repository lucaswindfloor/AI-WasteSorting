#include "remeasurement_schedule.h"

#include <algorithm>
#include <set>
#include <utility>

namespace waste::m06 {
namespace {

std::vector<Window> restrict_visibility(std::span<const Window> starts,
                                         const Exposure& exposure,
                                         const std::vector<Interval>& visibility) {
    std::vector<Window> result;
    for (const auto& allowed : visibility) {
        const Window start{std::max(Tick::zero(), allowed.begin - exposure.offset.begin),
                           allowed.end - exposure.offset.end};
        if (!valid(start)) {
            continue;
        }
        for (const auto& prior : starts) {
            if (auto shared = intersection(start, prior)) {
                result.push_back(*shared);
            }
        }
    }
    std::ranges::sort(result, {}, &Window::open);
    std::vector<Window> merged;
    for (const auto& next : result) {
        if (!merged.empty() && next.open <= merged.back().close) {
            merged.back().close = std::max(merged.back().close, next.close);
        } else {
            merged.push_back(next);
        }
    }
    return merged;
}

}  // namespace

std::optional<AppointmentPlan> plan_remeasurement(const SchedulingConfig& c,
                                                  const RemeasurementBudget& b,
                                                  const m05::Remeasurement& request,
                                                  const ExecutionSnapshot& snapshot,
                                                  std::span<const ExposureWindow> visibility,
                                                  const Deadlines& downstream,
                                                  std::string request_id, Tick now) {
    if (!c.enable_e11 || !snapshot_valid(c, snapshot) || request.target.generation != c.generation ||
        request.snapshot_revision != snapshot.eligibility.revision || request.required.empty() ||
        request.required.size() > 3 ||
        target_in_use(snapshot, target_key(request.target)) || now < request.assessed ||
        now >= request.valid_until || now >= c.valid_until || request_id.empty() || b.camera.empty() ||
        b.exposures.empty() || b.exposures.size() > c.max_resources ||
        visibility.size() > c.max_resources || b.camera_busy_upper < b.first_to_result_upper ||
        !valid(downstream.handoff)) {
        return {};
    }
    const auto total = add_ticks(b.first_to_result_upper, b.verification_and_recheck_upper);
    const auto latest_ready = std::min({downstream.handoff.close, request.valid_until - Tick(1),
                                       c.valid_until - Tick(1)});
    if (!total || latest_ready < *total || latest_ready - *total < now) {
        return {};
    }
    std::vector<Window> starts{{now, latest_ready - *total}};
    auto limits = c.max_intervals;
    std::set<std::string> components;
    std::set<m05::MissingComponent> provided;
    Tick previous_end{};
    for (const auto& exposure : b.exposures) {
        if (exposure.component.empty() || !components.insert(exposure.component).second ||
            !valid(exposure.offset) || exposure.offset.begin < previous_end ||
            (previous_end == Tick::zero() && exposure.offset.begin != Tick::zero()) ||
            exposure.offset.end > b.first_to_result_upper) {
            return {};
        }
        if (exposure.provides.size() > 3) {
            return {};
        }
        provided.insert(exposure.provides.begin(), exposure.provides.end());
        previous_end = exposure.offset.end;
        const auto it = std::ranges::find(visibility, exposure.component, &ExposureWindow::component);
        if (it == visibility.end() || it->visibility.empty() || it->visibility.size() > limits ||
            !std::ranges::all_of(it->visibility, [](auto interval) { return valid(interval); })) {
            return {};
        }
        limits -= it->visibility.size();
        starts = restrict_visibility(starts, exposure, it->visibility);
        if (starts.empty() || starts.size() > c.max_intervals) {
            return {};
        }
    }
    if (!std::ranges::all_of(request.required, [&](auto item) { return provided.contains(item); })) {
        return {};
    }
    const std::array required{OffsetUse{{ResourceKind::E11, b.camera}, {Tick::zero(), b.camera_busy_upper}}};
    for (const auto& start : starts) {
        const auto slot = find_resource_slot(snapshot, required, start, now);
        if (!slot.handoff) {
            continue;
        }
        const auto result_by = add_ticks(*slot.handoff, b.first_to_result_upper);
        const auto ready_by = add_ticks(*slot.handoff, *total);
        if (!result_by || !ready_by) {
            return {};
        }
        AppointmentPlan plan{std::move(request_id), request, snapshot.eligibility.revision, slot.uses.front(),
                             {}, *result_by, *ready_by, latest_ready};
        for (const auto& exposure : b.exposures) {
            // Bounded by result_by, checked above.
            plan.exposures.push_back({exposure.component,
                                       {*slot.handoff + exposure.offset.begin, *slot.handoff + exposure.offset.end},
                                       exposure.provides});
        }
        return plan;
    }
    return {};
}

AppointmentResult book_remeasurement(const AppointmentPlan& plan, RemeasurementPort& port, Tick now) {
    if (plan.request_id.empty() || plan.exposures.empty() || now > plan.camera.time.begin ||
        now >= plan.request.valid_until || plan.ready_by > plan.latest_ready) {
        return {{}, Reason::RemeasurementExpired};
    }
    try {
        auto reply = port.book(plan);
        if (reply.status == Reply::Accepted && !reply.appointment_id.empty()) {
            return {Appointment{plan, std::move(reply.appointment_id)}, Reason::None};
        }
        if ((reply.status == Reply::Conflict || reply.status == Reply::Rejected) &&
            reply.appointment_id.empty()) {
            return {{}, Reason::RemeasurementUnavailable};
        }
    } catch (...) {
    }
    return {{}, Reason::EffectUncertain};
}

RemeasurementResult recheck_remeasurement(const Appointment& appointment,
                                          const RemeasurementObservation& observation,
                                          const m05::EligibilityService& eligibility,
                                          const CurrentTarget& current,
                                          const m05::Snapshot& snapshot, Tick now) {
    const auto& plan = appointment.plan;
    const auto& proof = observation.proof;
    if (appointment.id.empty() || proof.appointment_id != appointment.id || plan.exposures.empty() ||
        proof.requested != plan.request.target || !proof.same_target_verified ||
        proof.measured < plan.exposures.front().offset.begin ||
        proof.measured > plan.exposures.back().offset.end ||
        proof.observed != m05::binding(current.target)) {
        return {{}, Reason::RemeasurementMismatch};
    }
    if (observation.exposures.size() != plan.exposures.size()) {
        return {{}, Reason::RemeasurementMismatch};
    }
    Tick last_exposure{};
    for (std::size_t i = 0; i < plan.exposures.size(); ++i) {
        const auto& actual = observation.exposures[i];
        const auto& planned = plan.exposures[i];
        if (actual.component != planned.component || !valid(actual.time) ||
            actual.time.begin < planned.offset.begin || actual.time.end > planned.offset.end) {
            return {{}, Reason::RemeasurementMismatch};
        }
        last_exposure = std::max(last_exposure, actual.time.end);
    }
    if (observation.available < last_exposure || observation.available < proof.measured ||
        observation.available > now) {
        return {{}, Reason::RemeasurementMismatch};
    }
    if (observation.available > plan.result_by || now > plan.latest_ready || now >= plan.request.valid_until || now >= proof.valid_until) {
        return {{}, Reason::RemeasurementExpired};
    }
    auto result = eligibility.evaluate_after_remeasurement(plan.request, proof, current.target, snapshot,
                                                            {current.grids, current.regions}, now);
    return {std::move(result), Reason::None};
}

}  // namespace waste::m06
