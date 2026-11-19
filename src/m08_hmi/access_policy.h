#pragma once

#include "hmi_types.h"

namespace waste::m08 {

bool allows(const Principal& principal, const Scope& scope, Permission permission);
bool allows(const Principal& principal, const Scope& scope, Action action);
bool can_read(const Principal& principal, const Operation& operation);

}  // namespace waste::m08
