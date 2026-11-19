#include "bin_router.h"

#include <algorithm>

namespace waste::m05 {
namespace {

BinCheck check_bin(const BinIdentity& expected, const Snapshot& snapshot, Tick now, Tick max_age) {
    const auto found = std::ranges::find_if(snapshot.bins, [&](const auto& item) {
        return item.identity.slot == expected.slot;
    });
    if (found == snapshot.bins.end()) {
        return {expected, {}, {State::Pending, Reason::BinNotReported, {}}};
    }
    const auto until = usable_until(found->time, now, max_age);
    if (until == Tick::zero()) {
        return {expected, *found, {State::Pending, Reason::BinStale, {}}};
    }
    Check check{State::Pending, Reason::BinNotReported, until};
    switch (found->condition) {
    case BinCondition::Ready:
        check = found->placement_revision && found->identity == expected
                ? Check{State::Ready, Reason::None, until}
                : Check{State::Pending, Reason::BinIdentityMismatch, until};
        break;
    case BinCondition::Absent: check = {State::Rejected, Reason::BinAbsent, until}; break;
    case BinCondition::Full: check = {State::Rejected, Reason::BinFull, until}; break;
    case BinCondition::Changing: check = {State::Pending, Reason::BinChanging, until}; break;
    case BinCondition::Unavailable: check = {State::Rejected, Reason::BinUnavailable, until}; break;
    case BinCondition::Unknown: break;
    }
    if ((found->condition == BinCondition::Ready || found->condition == BinCondition::Full ||
         found->condition == BinCondition::Unavailable) &&
        (found->identity != expected || !found->placement_revision)) {
        check = {State::Pending, Reason::BinIdentityMismatch, until};
    }
    return {expected, *found, check};
}

bool can_skip(const MaterialRule& rule, const BinCheck& bin) {
    if (!bin.observed || (bin.check.reason != Reason::BinAbsent && bin.check.reason != Reason::BinFull &&
                          bin.check.reason != Reason::BinChanging && bin.check.reason != Reason::BinUnavailable)) {
        return false;
    }
    return std::ranges::find(rule.fallback_after, bin.observed->condition) != rule.fallback_after.end();
}

}  // namespace

BinRoute route_bin(const MaterialRule& rule, const Snapshot& snapshot, Tick now, Tick max_age) {
    BinRoute result{{State::Rejected, Reason::NoBin, {}}, {}, {}};
    for (const auto& identity : rule.bins) {
        result.checks.push_back(check_bin(identity, snapshot, now, max_age));
    }
    auto until = Tick::max();
    for (std::size_t i = 0; i < result.checks.size(); ++i) {
        const auto& option = result.checks[i];
        until = std::min(until, option.check.valid_until);
        if (option.check.state == State::Ready) {
            result.selected = i;
            result.check = {State::Ready, Reason::None, until};
            return result;
        }
        if (i + 1 == result.checks.size()) {
            result.check = option.check;
            return result;
        }
        if (!can_skip(rule, option)) {
            result.check = option.check;
            if (option.check.state == State::Rejected) {
                result.check.reason = Reason::FallbackNotAuthorized;
            }
            return result;
        }
    }
    return result;
}

}  // namespace waste::m05
