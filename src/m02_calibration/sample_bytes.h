#pragma once

#include "calibration_types.h"

#include <bit>

namespace waste::m02 {

inline std::optional<std::span<const std::byte>> byte_range(const m01::Observation& observation,
                                                            m01::ByteRange range) {
    if (!observation.payload || range.offset > observation.payload->size() ||
        range.size > observation.payload->size() - range.offset) {
        return {};
    }
    return std::span(*observation.payload).subspan(range.offset, range.size);
}

inline std::uint32_t octet(std::byte value) {
    return std::to_integer<std::uint32_t>(value);
}

inline float f32_le(std::span<const std::byte> bytes) {
    const auto bits = octet(bytes[0]) | (octet(bytes[1]) << 8) |
                      (octet(bytes[2]) << 16) | (octet(bytes[3]) << 24);
    return std::bit_cast<float>(bits);
}

}  // namespace waste::m02
