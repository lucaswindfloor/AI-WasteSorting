#pragma once

#include "command_adapter.h"

namespace waste::m07 {

Issue reduce_feedback(AttemptRecord& record, const Feedback& event, const ExecutionConfig& config,
                      const FieldState* field, Tick now, bool authenticated_history = false);
void refresh_stage(AttemptRecord& record, const ExecutionConfig& config,
                   const FieldState* field, Tick now);

}  // namespace waste::m07
