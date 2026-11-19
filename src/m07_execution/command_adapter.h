#pragma once

#include "reservation_book.h"

namespace waste::m07 {

std::optional<m06::Window> applicable_receipt(const m06::Task& task);
Payload payload_from(const m06::Task& task);
bool same_handoff(const m06::Task& a, const m06::Task& b);
Issue check_task(const ExecutionConfig& config, const DurableState& state,
                 const AttemptRecord& record, const m06::Task& task, Tick now);
std::optional<Command> command_for(const AttemptRecord& record, Operation operation);

}  // namespace waste::m07
