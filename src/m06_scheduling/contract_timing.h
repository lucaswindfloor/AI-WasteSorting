#pragma once

#include "scheduling_config.h"

namespace waste::m06 {

std::optional<Deadlines> contract_deadlines(Contract contract, const Budget& budget,
                                            Window contact, std::optional<Tick> receipt_expiry = {});
Reason validate_plan(const Task& task, const PlanEnvelope& plan, Tick now,
                     Tick remaining_commit_to_receipt_upper);

}  // namespace waste::m06
