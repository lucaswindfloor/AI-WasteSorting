#pragma once

#include "scheduling_config.h"

namespace waste::m06 {

struct ResourceSearch {
    std::optional<Tick> handoff;
    std::vector<ResourceUse> uses;
    Reason reason{Reason::None};
};

bool snapshot_valid(const SchedulingConfig& config, const ExecutionSnapshot& snapshot);
bool target_in_use(const ExecutionSnapshot& snapshot, const TargetKey& target);
std::vector<OffsetUse> required_resources(const Lane& lane, const m05::Candidate& candidate);
ResourceSearch find_resource_slot(const ExecutionSnapshot& snapshot, std::span<const OffsetUse> required,
                                  Window handoff, Tick now, std::string_view own_token = {});

}  // namespace waste::m06
