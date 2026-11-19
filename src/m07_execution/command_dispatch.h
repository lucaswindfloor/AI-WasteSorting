#pragma once

#include "controller_port.h"
#include "command_adapter.h"

namespace waste::m07 {

std::optional<Operation> next_operation(const AttemptRecord& attempt);
Issue send_gate(const ExecutionConfig& config, const DurableState& state,
                const AttemptRecord& attempt, Operation operation, Tick now);
void apply_transport(AttemptRecord& attempt, Operation operation, Transport result, Tick now);

}  // namespace waste::m07
