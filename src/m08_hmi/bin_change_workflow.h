#pragma once

#include "state_projection.h"

namespace waste::m08 {

Transition bin_transition(const Settings& settings, const Operation& operation, const Runtime& runtime, Tick now);

}  // namespace waste::m08
