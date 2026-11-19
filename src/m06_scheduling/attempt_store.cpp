#include "attempt_store.h"

#include <limits>
#include <utility>

namespace waste::m06 {

AttemptStore::AttemptStore(std::string session, std::size_t capacity)
    : session_(std::move(session)), capacity_(capacity) {}

Attempt* AttemptStore::open(const TargetKey& target, Tick target_until) {
    if (active_.contains(target) || active_.size() >= capacity_ ||
        sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return nullptr;
    }
    auto entry = active_.emplace(target, Attempt{{target, session_, ++sequence_},
                                                AttemptPhase::Reserving, {}, target_until});
    return &entry.first->second;
}

const Attempt* AttemptStore::find(const TargetKey& target) const {
    const auto it = active_.find(target);
    return it == active_.end() ? nullptr : &it->second;
}

void AttemptStore::settle(const TargetKey& target) {
    active_.erase(target);
}

void AttemptStore::reconcile(ExecutionPort& execution, Tick now) {
    for (auto it = active_.begin(); it != active_.end();) {
        try {
            auto& attempt = it->second;
            if (attempt.phase == AttemptPhase::Finished) {
                if (now >= attempt.target_until) {
                    it = active_.erase(it);
                    continue;
                }
                ++it;
                continue;
            }
            const auto status = execution.status(attempt.id);
            if (status == AttemptStatus::ClosedUnused) {
                it = active_.erase(it);
                continue;
            }
            if (status == AttemptStatus::ClosedFinished) {
                attempt.phase = AttemptPhase::Finished;
            } else if (status == AttemptStatus::Delivered || status == AttemptStatus::Active) {
                attempt.phase = AttemptPhase::HandedOff;
            }
        } catch (...) {
            // A failed query proves nothing about external effects.
        }
        ++it;
    }
}

}  // namespace waste::m06
