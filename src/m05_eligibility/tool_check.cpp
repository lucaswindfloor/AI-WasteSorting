#include "tool_check.h"
#include <algorithm>

namespace waste::m05 {

ToolCheck check_tool(const ToolIdentity& expected, const Snapshot& snapshot, Tick now, Tick max_age) {
    const auto found = std::ranges::find_if(snapshot.tools, [&](const auto& item) {
        return item.identity.station == expected.station;
    });
    if (found == snapshot.tools.end()) {
        return {expected, {}, {State::Pending, Reason::ToolNotReported, {}}};
    }
    const auto until = usable_until(found->time, now, max_age);
    Check check{State::Ready, Reason::None, until};
    if (until == Tick::zero()) {
        check = {State::Pending, Reason::ToolStale, {}};
    } else if (found->condition == ToolCondition::Changing) {
        check = {State::Pending, Reason::ToolChanging, until};
    } else if (found->condition == ToolCondition::Unavailable || found->condition == ToolCondition::Fault) {
        check = {State::Rejected, Reason::ToolUnavailable, until};
    } else if (found->condition != ToolCondition::Ready) {
        check = {State::Pending, Reason::ToolNotReported, until};
    } else if (found->identity != expected) {
        check = {State::Pending, Reason::ToolMismatch, until};
    }
    return {expected, *found, check};
}

}  // namespace waste::m05
