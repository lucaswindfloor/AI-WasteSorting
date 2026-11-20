#include "storage_pressure.h"

#include <limits>

namespace waste::m09 {
namespace {

std::uint64_t add(std::uint64_t left, std::uint64_t right) {
    const auto max = std::numeric_limits<std::uint64_t>::max();
    return right > max - left ? max : left + right;
}

}  // namespace

Pressure assess_pressure(const PressureInput& in) {
    const auto usable = in.free_disk > in.command_reserve ? in.free_disk - in.command_reserve : 0;
    const auto tail = add(in.in_flight, in.tail);
    const auto required = add(in.queued, tail);
    return {usable, required, tail, usable <= required, in.buffer_free <= tail || usable <= required};
}

}  // namespace waste::m09
