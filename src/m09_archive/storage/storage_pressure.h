#pragma once

#include "../archive_types.h"

namespace waste::m09 {

struct PressureInput {
    std::uint64_t free_disk{}, command_reserve{}, queued{}, in_flight{}, tail{}, buffer_free{};
};

struct Pressure {
    std::uint64_t usable_disk{}, required_disk{}, required_buffer{};
    bool limit_raw{}, limit_input{};
};

Pressure assess_pressure(const PressureInput& input);

}  // namespace waste::m09
