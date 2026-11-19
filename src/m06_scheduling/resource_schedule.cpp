#include "resource_schedule.h"

#include <algorithm>
#include <set>

namespace waste::m06 {
namespace {

std::vector<Interval> merge_intervals(std::vector<Interval> intervals) {
    std::ranges::sort(intervals, {}, &Interval::begin);
    std::vector<Interval> merged;
    for (const auto& i : intervals) {
        if (!merged.empty() && i.begin <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, i.end);
        } else {
            merged.push_back(i);
        }
    }
    return merged;
}

std::vector<Interval> free_intervals(const std::vector<Interval>& usable,
                                     const std::vector<Interval>& occupied) {
    std::vector<Interval> free;
    for (const auto& i : usable) {
        auto current = i.begin;
        for (const auto& busy : occupied) {
            if (busy.end <= current) {
                continue;
            }
            if (busy.begin >= i.end) {
                break;
            }
            if (current < busy.begin) {
                free.push_back({current, busy.begin});
            }
            current = busy.end;
            if (current >= i.end) {
                break;
            }
        }
        if (current < i.end) {
            free.push_back({current, i.end});
        }
    }
    return free;
}

std::vector<Window> intersect_sets(std::span<const Window> a, std::span<const Window> b) {
    std::vector<Window> result;
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (auto span = intersection(a[i], b[j])) {
            result.push_back(*span);
        }
        if (a[i].close < b[j].close) {
            ++i;
        } else {
            ++j;
        }
    }
    return result;
}

bool proper_resource(const ResourceKey& resource) {
    return !resource.id.empty() &&
           (resource.kind == ResourceKind::Robot || resource.kind == ResourceKind::Tool ||
            resource.kind == ResourceKind::Bin || resource.kind == ResourceKind::E11);
}

}  // namespace

bool snapshot_valid(const SchedulingConfig& c, const ExecutionSnapshot& snapshot) {
    if (!snapshot.eligibility.revision || snapshot.eligibility.generation != c.generation ||
        snapshot.calendars.size() > c.max_resources || snapshot.reservations.size() > c.max_intervals ||
        snapshot.unresolved_targets.size() > c.max_intervals) {
        return false;
    }
    auto remaining = c.max_intervals;
    std::set<ResourceKey> resources;
    const auto valid_list = [&](const std::vector<Interval>& items) {
        if (items.size() > remaining || !std::ranges::all_of(items, [](auto i) { return valid(i); })) {
            return false;
        }
        remaining -= items.size();
        return true;
    };
    for (const auto& calendar : snapshot.calendars) {
        if (!proper_resource(calendar.resource) || !resources.insert(calendar.resource).second ||
            calendar.valid_until <= Tick::zero() || !valid_list(calendar.usable) ||
            !valid_list(calendar.occupied)) {
            return false;
        }
    }
    std::set<std::string> tokens;
    std::set<TargetKey> attempts;
    for (const auto& reservation : snapshot.reservations) {
        if (reservation.token.empty() || !tokens.insert(reservation.token).second ||
            reservation.attempt.target.run_id.empty() || !reservation.attempt.target.id ||
            reservation.attempt.scheduler_session.empty() || !reservation.attempt.sequence ||
            !attempts.insert(reservation.attempt.target).second ||
            reservation.valid_until <= Tick::zero() || reservation.uses.empty() ||
            reservation.uses.size() > remaining) {
            return false;
        }
        remaining -= reservation.uses.size();
        std::set<ResourceKey> seen;
        for (const auto& use : reservation.uses) {
            if (!proper_resource(use.resource) || !seen.insert(use.resource).second || !valid(use.time)) {
                return false;
            }
        }
    }
    return std::ranges::all_of(snapshot.unresolved_targets, [](const auto& target) {
        return !target.run_id.empty() && target.id != 0;
    });
}

bool target_in_use(const ExecutionSnapshot& snapshot, const TargetKey& target) {
    return std::ranges::find(snapshot.unresolved_targets, target) != snapshot.unresolved_targets.end() ||
           std::ranges::any_of(snapshot.reservations, [&](const auto& held) {
               return held.attempt.target == target;
           });
}

std::vector<OffsetUse> required_resources(const Lane& lane, const m05::Candidate& candidate) {
    // The physical tool and bin are stable aliases across station/slot changes.
    return {{{ResourceKind::Robot, lane.robot}, lane.robot_use},
            {{ResourceKind::Tool, candidate.tool.identity.tool}, lane.tool_use},
            {{ResourceKind::Bin, candidate.bin.identity.physical_bin}, lane.bin_use}};
}

ResourceSearch find_resource_slot(const ExecutionSnapshot& snapshot, std::span<const OffsetUse> required,
                                  Window handoff, Tick now, std::string_view own_token) {
    if (!valid(handoff) || required.empty() || now < Tick::zero() || handoff.open < now) {
        return {{}, {}, Reason::NoResourceSlot};
    }
    std::vector<Window> allowed{handoff};
    for (const auto& item : required) {
        if (!valid(item.offset)) {
            return {{}, {}, Reason::InvalidInput};
        }
        const auto it = std::ranges::find(snapshot.calendars, item.resource, &ResourceCalendar::resource);
        if (it == snapshot.calendars.end() || now >= it->valid_until || it->usable.empty()) {
            return {{}, {}, Reason::ResourceUnavailable};
        }
        auto occupied = it->occupied;
        for (const auto& reservation : snapshot.reservations) {
            if (!own_token.empty() && reservation.token == own_token) {
                continue;
            }
            for (const auto& use : reservation.uses) {
                if (use.resource == item.resource) {
                    occupied.push_back(use.time);
                }
            }
        }
        const auto free = free_intervals(merge_intervals(it->usable),
                                         merge_intervals(std::move(occupied)));
        std::vector<Window> starts;
        for (const auto& interval : free) {
            const Window window{std::max(Tick::zero(), interval.begin - item.offset.begin),
                                std::min(interval.end - item.offset.end, it->valid_until - Tick(1))};
            if (valid(window)) {
                starts.push_back(window);
            }
        }
        allowed = intersect_sets(allowed, starts);
        if (allowed.empty()) {
            return {{}, {}, Reason::NoResourceSlot};
        }
    }
    ResourceSearch result{allowed.front().open, {}, Reason::None};
    for (const auto& item : required) {
        const auto begin = add_ticks(*result.handoff, item.offset.begin);
        const auto end = add_ticks(*result.handoff, item.offset.end);
        if (!begin || !end) {
            return {{}, {}, Reason::NoResourceSlot};
        }
        result.uses.push_back({item.resource, {*begin, *end}});
    }
    return result;
}

}  // namespace waste::m06
