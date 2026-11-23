#pragma once

#include "m01_acquisition/acquisition_types.h"

#include <array>
#include <span>
#include <string_view>

namespace waste::m01sim {

constexpr double belt_width = 240;
constexpr double scan_x = 142;
constexpr double travel = 360;
constexpr std::int64_t encoder_counts_per_line = 30;
constexpr double pitch_mm = 1.5;
constexpr unsigned trigger_count = 240;
constexpr unsigned preview_width = 64;

struct Material {
    std::string_view id, kind;
    double x, y, length, width, height;
};

struct ProfileStation {
    double along, width_fraction;
};

inline constexpr std::array materials{
    Material{"P01", "PET", 18, 80, 94, 48, 48.6},
    Material{"A02", "AL", -117, 166, 67, 39, 31.0},
};

std::span<const ProfileStation> footprint(const Material& item);

struct Profile {
    unsigned rgb_width{96}, hsi_width{80}, hsi_bands{16}, depth_width{64}, packet_lines{2};
};

struct Scan {
    std::vector<std::byte> bytes;
    std::size_t data_bytes{};
    std::optional<m01::ByteRange> mask;
};

Scan scan(m01::Source source, const Profile& profile, double displacement, bool mask_depth, double sensor_x = scan_x);
std::vector<std::string> wavelengths(unsigned bands);
std::vector<int> preview(const m01::Observation& observation, const Profile& profile);

}  // namespace waste::m01sim
