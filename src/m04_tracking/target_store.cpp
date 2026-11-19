#include "target_store.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace waste::m04 {

std::optional<TargetId> TargetStore::insert(const m03::Generation& generation, Footprint footprint,
                                           Tick expiry, std::size_t limit) {
    if (targets_.size() >= limit || last_id_ == std::numeric_limits<TargetId>::max()) {
        return {};
    }
    const auto id = ++last_id_;
    const auto association_time = footprint.reference.time;
    targets_.emplace(id, Target{id, 0, generation, TargetState::Tracked, Reason::None,
                                std::move(footprint), {}, {}, expiry, association_time});
    changed(id);
    return id;
}

const Target* TargetStore::find(TargetId id) const {
    const auto result = targets_.find(id);
    return result == targets_.end() ? nullptr : &result->second;
}

Target* TargetStore::find(TargetId id) {
    const auto result = targets_.find(id);
    return result == targets_.end() ? nullptr : &result->second;
}

std::vector<const Target*> TargetStore::active() const {
    std::vector<const Target*> result;
    for (const auto& [id, target] : targets_) {
        if (target.state != TargetState::Invalid) {
            result.push_back(&target);
        }
    }
    return result;
}

void TargetStore::changed(TargetId id) {
    dirty_.insert(id);
}

void TargetStore::invalidate(TargetId id, Reason reason) {
    if (auto* target = find(id); target && target->state != TargetState::Invalid) {
        target->state = TargetState::Invalid;
        target->reason = reason;
        changed(id);
    }
}

void TargetStore::publish(Report& report) {
    for (auto id : dirty_) {
        auto& target = targets_.at(id);
        if (target.version == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("target version exhausted");
        }
        ++target.version;
        report.targets.push_back(target);
        if (target.state == TargetState::Invalid) {
            targets_.erase(id);
        }
    }
    dirty_.clear();
}

}  // namespace waste::m04
