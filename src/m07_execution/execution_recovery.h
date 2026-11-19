#pragma once

#include "event_reducer.h"
#include "controller_port.h"

namespace waste::m07 {

bool compatible_checkpoint(const DurableState& state, const ExecutionConfig& config, Tick now);
void mark_restarted(DurableState& state);
const Intent* original_request(const AttemptRecord& attempt);
Issue merge_query(AttemptRecord& attempt, const QueryReply& reply, const ExecutionConfig& config,
                  const FieldState* field, Tick now);

}  // namespace waste::m07
