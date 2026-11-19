#include "scheduling_config.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace waste::m06 {

std::optional<Tick> add_ticks(Tick a, Tick b) {
    if (a < Tick::zero() || b < Tick::zero() || a > Tick::max() - b) {
        return {};
    }
    return a + b;
}

bool valid(Window w) { return w.open >= Tick::zero() && w.close >= w.open; }
bool valid(Interval i) { return i.begin >= Tick::zero() && i.end > i.begin; }

std::optional<Window> intersection(Window a, Window b) {
    const Window result{std::max(a.open, b.open), std::min(a.close, b.close)};
    return valid(result) ? std::optional(result) : std::nullopt;
}

void validate(const SchedulingConfig& c) {
    if (c.generation.run_id.empty() || c.version.empty() || c.scheduler_session.empty() ||
        c.valid_until <= Tick::zero() || c.max_prediction_age <= Tick::zero() ||
        c.transaction_lead < Tick::zero() || !std::isfinite(c.max_tracking_error_mm) ||
        c.max_tracking_error_mm <= 0 || !std::isfinite(c.forecast_extra_error_mm) ||
        c.forecast_extra_error_mm < 0 || !c.max_batch || c.max_batch > 1024 ||
        !c.max_attempts || c.max_attempts > 1024 || !c.max_intervals || c.max_intervals > 4096 ||
        !c.max_resources || c.max_resources > 256 || c.max_retries > 16 ||
        c.lanes.empty() || c.lanes.size() > c.max_resources) {
        throw std::invalid_argument("invalid scheduling identity, limits or time bounds");
    }
    std::set<std::string> stations;
    for (const auto& lane : c.lanes) {
        const auto& b = lane.budget;
        const auto latest_contact = add_ticks(b.handoff_to_receipt_max, b.receipt_to_contact_max);
        if (lane.station.empty() || lane.robot.empty() || !stations.insert(lane.station).second ||
            lane.contact_regions.empty() || lane.contact_regions.size() > c.max_intervals ||
            b.handoff_to_receipt_min < Tick::zero() || b.handoff_to_receipt_max < b.handoff_to_receipt_min ||
            b.receipt_to_contact_min < Tick::zero() || b.receipt_to_contact_max < b.receipt_to_contact_min ||
            b.early_margin < Tick::zero() || b.late_margin < Tick::zero() || !latest_contact ||
            (lane.contract != Contract::A && lane.contract != Contract::B) ||
            (lane.tracking != TrackingMode::ControllerNative && lane.tracking != TrackingMode::ExternalReference) ||
            !valid(lane.robot_use) || !valid(lane.tool_use) || !valid(lane.bin_use) ||
            lane.robot_use.begin != Tick::zero() || lane.tool_use.begin != Tick::zero() ||
            lane.robot_use.end <= *latest_contact || lane.tool_use.end <= *latest_contact ||
            lane.bin_use.end <= *latest_contact || lane.bin_use.end > lane.robot_use.end || lane.bin_use.end > lane.tool_use.end) {
            throw std::invalid_argument("invalid lane, contract or full occupancy bound");
        }
        std::set<std::string> regions;
        for (const auto& region : lane.contact_regions) {
            const auto& lo = region.min;
            const auto& hi = region.max;
            if (region.id.empty() || !regions.insert(region.id).second ||
                !std::isfinite(lo.x) || !std::isfinite(lo.y) || !std::isfinite(lo.z) ||
                !std::isfinite(hi.x) || !std::isfinite(hi.y) || !std::isfinite(hi.z) ||
                lo.x >= hi.x || lo.y > hi.y || lo.z > hi.z) {
                throw std::invalid_argument("invalid contact region");
            }
        }
    }
}

const Lane* find_lane(const SchedulingConfig& c, std::string_view station) {
    const auto found = std::ranges::find_if(c.lanes, [&](const auto& lane) { return lane.station == station; });
    return found == c.lanes.end() ? nullptr : &*found;
}

}  // namespace waste::m06
