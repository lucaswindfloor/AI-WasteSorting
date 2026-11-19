#pragma once

#include "state_projection.h"

namespace waste::m08 {

Transition config_transition(const Settings& settings, const Journal& journal, const Operation& operation,
                             const Runtime& runtime, Tick now);
bool describes_deployment(const Deployment& fact, const Package& package, const Operation& operation,
                           Tick now, Tick max_age);

}  // namespace waste::m08
