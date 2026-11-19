#pragma once

#include "tracking_types.h"

#include <map>
#include <set>

namespace waste::m04 {

class TargetStore {
public:
    std::optional<TargetId> insert(const m03::Generation& generation, Footprint footprint,
                                   Tick expiry, std::size_t limit);
    const Target* find(TargetId id) const;
    Target* find(TargetId id);
    std::vector<const Target*> active() const;
    void changed(TargetId id);
    void invalidate(TargetId id, Reason reason);
    void publish(Report& report);

private:
    TargetId last_id_{};
    std::map<TargetId, Target> targets_;
    std::set<TargetId> dirty_;
};

}  // namespace waste::m04
