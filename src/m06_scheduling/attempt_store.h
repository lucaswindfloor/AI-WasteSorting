#pragma once

#include "execution_port.h"

#include <map>

namespace waste::m06 {

enum class AttemptPhase { Reserving, Reserved, HandedOff, Uncertain, Finished };

struct Attempt {
    AttemptId id;
    AttemptPhase phase{AttemptPhase::Reserving};
    std::string token;
    Tick target_until{};
};

class AttemptStore {
public:
    AttemptStore(std::string session, std::size_t capacity);
    Attempt* open(const TargetKey& target, Tick target_until);
    const Attempt* find(const TargetKey& target) const;
    void settle(const TargetKey& target);
    void reconcile(ExecutionPort& execution, Tick now);

private:
    std::string session_;
    std::size_t capacity_;
    std::uint64_t sequence_{};
    std::map<TargetKey, Attempt> active_;
};

}  // namespace waste::m06
