#pragma once

#include "access_policy.h"

namespace waste::m08 {

bool fresh(const Stamp& stamp, Tick now, Tick max_age);
bool terminal(Phase phase);
const Bin* find_bin(const Runtime& runtime, std::string_view slot);
bool active_deployment(const Settings& settings, const Runtime& runtime, Tick now);
Issue preflight(const Settings& settings, const Journal& journal, const Runtime& runtime,
                const Request& request, Tick now);
Workbench project(const Settings& settings, const Journal& journal, const Runtime& runtime,
                  const Principal& principal, bool operational, Tick now);

}  // namespace waste::m08
