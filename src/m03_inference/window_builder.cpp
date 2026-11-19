#include "window_builder.h"

#include <algorithm>
#include <limits>

namespace waste::m03 {
namespace {
constexpr auto max_trigger = std::numeric_limits<std::uint64_t>::max();
}

WindowBuilder::WindowBuilder(Generation generation, const Manifest& model, std::uint64_t first_trigger)
    : generation_(std::move(generation)), model_(model.identity.key), spec_(model.window), next_(first_trigger) {}

WindowId WindowBuilder::id(std::uint64_t last) const {
    return {generation_, model_, next_, last};
}

bool WindowBuilder::in_range(std::uint64_t trigger) const {
    return trigger >= next_ && trigger - next_ < spec_.max_buffered_rows;
}

Insert WindowBuilder::add(std::uint64_t trigger, InputRow row) {
    if (ended_ || trigger < next_ || gaps_.contains(trigger) || (sealed_ && trigger <= *sealed_)) {
        return Insert::Closed;
    }
    if (!in_range(trigger)) {
        return Insert::OutOfRange;
    }
    rows_.try_emplace(trigger, std::move(row));
    horizon_ = std::max(horizon_.value_or(trigger), trigger);
    return Insert::Accepted;
}

Insert WindowBuilder::mark_gap(std::uint64_t first, std::uint64_t last) {
    if (ended_ || last < next_) {
        return Insert::Closed;
    }
    if (first > last || !in_range(last)) {
        return Insert::OutOfRange;
    }
    for (auto trigger = std::max(first, next_);; ++trigger) {
        // A conflicting late gap cannot erase a completed source record.
        if (!rows_.contains(trigger)) {
            gaps_.insert(trigger);
        }
        if (trigger == last) {
            break;
        }
    }
    horizon_ = std::max(horizon_.value_or(last), last);
    return Insert::Accepted;
}

Insert WindowBuilder::seal_through(std::uint64_t trigger) {
    if (ended_ || trigger < next_) {
        return Insert::Closed;
    }
    if (!in_range(trigger)) {
        return Insert::OutOfRange;
    }
    sealed_ = std::max(sealed_.value_or(trigger), trigger);
    horizon_ = std::max(horizon_.value_or(trigger), trigger);
    return Insert::Accepted;
}

std::optional<Window> WindowBuilder::take(Tick now) {
    if (ended_ || !horizon_ || next_ > *horizon_ || spec_.rows - 1 > max_trigger - next_) {
        return {};
    }
    const auto last = next_ + spec_.rows - 1;
    auto first_received = Tick::max();
    auto deadline = Tick::max();
    for (auto row = rows_.lower_bound(next_); row != rows_.end() && row->first <= last; ++row) {
        first_received = std::min(first_received, row->second.received);
        deadline = std::min(deadline, row->second.valid_until);
    }
    const bool expired = first_received != Tick::max() &&
                         (now >= deadline || now - first_received >= spec_.max_wait);
    const auto settled = [&](std::uint64_t trigger) {
        return (sealed_ && trigger <= *sealed_) || rows_.contains(trigger) || gaps_.contains(trigger);
    };
    bool complete = last <= *horizon_;
    for (std::size_t i = 0; complete && i < spec_.rows; ++i) {
        complete = settled(next_ + i);
    }
    // A timed-out partial window preserves its nominal row grid.
    if (!complete && !expired) {
        return {};
    }
    Window window{id(last), {}, first_received == Tick::max() ? now : first_received, now, deadline};
    window.rows.resize(spec_.rows);
    for (auto row = rows_.lower_bound(next_); row != rows_.end() && row->first <= last; ++row) {
        window.rows[row->first - next_] = row->second;
    }
    if (spec_.step > max_trigger - next_) {
        ended_ = true;
        rows_.clear();
        gaps_.clear();
    } else {
        next_ += spec_.step;
        rows_.erase(rows_.begin(), rows_.lower_bound(next_));
        gaps_.erase(gaps_.begin(), gaps_.lower_bound(next_));
    }
    return window;
}

std::optional<WindowId> WindowBuilder::unfinished() const {
    if (!ended_ && horizon_ && next_ <= *horizon_) {
        return id(*horizon_);
    }
    return {};
}

}  // namespace waste::m03
