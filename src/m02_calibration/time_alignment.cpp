#include "time_alignment.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace waste::m02 {
namespace {

std::optional<m01::Tick> nonnegative_tick(long double ns) {
    const auto end = std::ldexp(1.0L, std::numeric_limits<m01::Tick::rep>::digits);
    const auto rounded = std::round(ns);
    if (!std::isfinite(ns) || ns < 0 || rounded >= end) {
        return {};
    }
    return m01::Tick(static_cast<m01::Tick::rep>(rounded));
}

std::optional<TimeEstimate> map(const Snapshot& snapshot, m01::Source source,
                                std::uint64_t session, const m01::SourceTime& time,
                                std::string_view basis) {
    const auto mapping = std::ranges::find_if(snapshot.clocks, [&](const auto& value) {
        return value.source == source && value.session == session && value.domain == time.clock_domain &&
               value.time_basis == basis && time.ticks >= value.first_tick && time.ticks <= value.last_tick;
    });
    if (mapping == snapshot.clocks.end()) {
        return {};
    }
    const auto host = nonnegative_tick(mapping->host_anchor.count() +
                     (static_cast<long double>(time.ticks) - mapping->source_anchor) * mapping->ns_per_tick);
    const auto error = nonnegative_tick(static_cast<long double>(mapping->error.count()) + 1);
    if (!host || !error) {
        return {};
    }
    return TimeEstimate{*host, *error, TimeBasis::SourceClock};
}

}  // namespace

std::optional<TimeEstimate> sync_time(const Snapshot& snapshot, const m01::TriggerEvent& trigger) {
    if (!trigger.source_time || trigger.key.encoder_epoch != snapshot.encoder.epoch) {
        return {};
    }
    return map(snapshot, m01::Source::Sync, trigger.key.sync_session, *trigger.source_time, "trigger");
}

std::optional<TimeEstimate> observation_time(const Snapshot& snapshot,
                                             const m01::ObservationHeader& header,
                                             const m01::TriggerRecord* trigger) {
    if (header.source_time) {
        if (auto time = map(snapshot, header.id.source, header.id.session, *header.source_time,
                            header.time_basis)) {
            return time;
        }
    }
    if (!trigger || !header.association_verified || header.trigger != trigger->event.key) {
        return {};
    }
    const auto delay = std::ranges::find_if(snapshot.trigger_delays, [&](const auto& value) {
        return value.source == header.id.source && value.session == header.id.session;
    });
    const auto base = sync_time(snapshot, trigger->event);
    if (delay == snapshot.trigger_delays.end() || !base) {
        return {};
    }
    const auto time = nonnegative_tick(static_cast<long double>(base->value.count()) + delay->offset.count());
    const auto error = nonnegative_tick(static_cast<long double>(base->error.count()) + delay->error.count());
    if (!time || !error) {
        return {};
    }
    return TimeEstimate{*time, *error, TimeBasis::TriggerDelay};
}

bool within_time(const TimeEstimate& a, const TimeEstimate& b, m01::Tick limit) {
    const auto difference = std::abs(static_cast<long double>(a.value.count()) - b.value.count());
    return a.error >= m01::Tick::zero() && b.error >= m01::Tick::zero() &&
           difference + a.error.count() + b.error.count() <= static_cast<long double>(limit.count());
}

}  // namespace waste::m02
