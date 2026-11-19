#pragma once

#include "execution_config.h"

namespace waste::m07 {

Issue check_reservation(const DurableState& state, const ExecutionConfig& config,
                        const m06::ReservationRequest& request, Tick now);
m06::ExecutionSnapshot build_snapshot(const DurableState& state, const ExecutionConfig& config, Tick now);
bool has_live_effect(const AttemptRecord& record);
m06::AttemptStatus status_of(Stage stage);

}  // namespace waste::m07
