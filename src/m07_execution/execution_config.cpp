#include "execution_config.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace waste::m07 {
namespace {

bool fresh(Tick observed, Tick expires, Tick now, Tick max_age) {
    return observed >= Tick::zero() && observed <= now && now < expires && now - observed < max_age;
}

bool proper_gate(const Gate& gate, std::string_view configured, Tick now) {
    return gate.source == configured && gate.observed >= Tick::zero() && gate.observed <= now &&
           gate.valid_until > gate.observed;
}

}  // namespace

void validate(const ExecutionConfig& c) {
    if (c.generation.run_id.empty() || c.version.empty() || c.writer_session.empty() ||
        c.process_source.empty() || c.safety_source.empty() || c.valid_until <= Tick::zero() ||
        c.max_field_age <= Tick::zero() || c.max_event_age <= Tick::zero() ||
        c.query_interval <= Tick::zero() || c.lanes.empty() || !c.max_attempts || c.max_attempts > 4096 ||
        !c.max_events_per_attempt || c.max_events_per_attempt > 1024 ||
        !c.max_wire_bytes || c.max_wire_bytes > 1024 * 1024 ||
        !c.max_resources || c.max_resources > 256 || c.lanes.size() > c.max_resources ||
        !c.max_intervals || c.max_intervals > 8192) {
        throw std::invalid_argument("invalid execution configuration");
    }
    std::set<std::string> stations;
    for (const auto& lane : c.lanes) {
        if (lane.station.empty() || lane.robot.empty() || lane.controller.empty() || lane.terminal.empty() ||
            !stations.insert(lane.station).second || lane.send_to_receipt_upper < Tick::zero() ||
            (lane.contract != m06::Contract::A && lane.contract != m06::Contract::B) ||
            (lane.contract == m06::Contract::A && !lane.prepare_is_planning_only) ||
            (lane.tracking != m06::TrackingMode::ControllerNative &&
             lane.tracking != m06::TrackingMode::ExternalReference)) {
            throw std::invalid_argument("controller lane or its approved contract is invalid");
        }
    }
}

const Lane* lane_for(const ExecutionConfig& config, std::string_view station) {
    const auto match = std::ranges::find_if(config.lanes, [&](const auto& lane) {
        return lane.station == station;
    });
    return match == config.lanes.end() ? nullptr : &*match;
}

const DeviceLink* current_link(std::span<const DeviceLink> links, std::string_view id, Tick now,
                               Tick max_age) {
    const auto link = std::ranges::find_if(links, [&](const auto& l) { return l.device == id; });
    if (link == links.end() || !link->available ||
        !fresh(link->observed, link->valid_until, now, max_age)) {
        return nullptr;
    }
    return &*link;
}

bool gate_open(const ExecutionConfig& c, const FieldState* f, Tick now) {
    return f && now < c.valid_until && f->process.permits && f->safety_mirror.permits &&
           fresh(f->process.observed, f->process.valid_until, now, c.max_field_age) &&
           fresh(f->safety_mirror.observed, f->safety_mirror.valid_until, now, c.max_field_age) &&
           fresh(f->observed, Tick::max(), now, c.max_field_age);
}

bool valid_field(const ExecutionConfig& c, const FieldState& f, Tick now) {
    if (f.eligibility.generation != c.generation || !f.sequence || f.observed < Tick::zero() ||
        f.observed > now || f.eligibility.bin_mapping_version.empty() ||
        f.calendars.size() > c.max_resources || f.controllers.size() > c.max_resources ||
        f.terminals.size() > c.max_resources || f.eligibility.tools.size() > c.max_resources ||
        f.eligibility.bins.size() > c.max_resources ||
        !proper_gate(f.process, c.process_source, now) ||
        !proper_gate(f.safety_mirror, c.safety_source, now)) {
        return false;
    }
    const auto valid_links = [&](const auto& links) {
        std::set<std::string> ids;
        for (const auto& l : links) {
            if (l.device.empty() || (l.available && l.session.empty()) || !ids.insert(l.device).second ||
                l.observed < Tick::zero() || l.observed > now || l.valid_until <= l.observed) {
                return false;
            }
        }
        return true;
    };
    auto remaining = c.max_intervals;
    std::set<m06::ResourceKey> keys;
    const auto intervals = [&](const auto& list) {
        if (list.size() > remaining ||
            !std::ranges::all_of(list, [](auto window) { return m06::valid(window); })) {
            return false;
        }
        remaining -= list.size();
        return true;
    };
    for (const auto& calendar : f.calendars) {
        if (calendar.resource.id.empty() || !keys.insert(calendar.resource).second ||
            calendar.valid_until <= Tick::zero() || !intervals(calendar.usable) || !intervals(calendar.occupied)) {
            return false;
        }
    }
    return valid_links(f.controllers) && valid_links(f.terminals);
}

}  // namespace waste::m07
