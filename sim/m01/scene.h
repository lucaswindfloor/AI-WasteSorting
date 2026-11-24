#pragma once

#include "m01_acquisition/acquisition_types.h"

#include <array>
#include <span>
#include <string_view>

namespace waste::m01sim {

constexpr double belt_width = 240;
constexpr double scan_x = 142;
constexpr double pitch_mm = 1.5;
constexpr unsigned trigger_count = 240;
constexpr unsigned preview_width = 64;
constexpr unsigned production_frame_rows = 128;
constexpr unsigned production_window_rows = 512;

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

inline constexpr std::array production_materials{
    Material{"P01", "PET", 250, 335, 210, 66, 66},
    Material{"A02", "AL", 125, 650, 122, 66, 66},
};

struct Layout {
    double width, length, rgb_x, hsi_x, depth_x, pitch;
    unsigned lines;
    double pick_begin, pick_end;
    std::span<const Material> objects;

    auto encoder_step() const { return static_cast<std::int64_t>(pitch * 20); }
};

inline constexpr Layout demo_layout{belt_width, 0, scan_x, scan_x, scan_x, pitch_mm, trigger_count, 0, 0, materials};
inline constexpr double production_scan_x = 750;
inline constexpr Layout production_layout{800, 5500, production_scan_x, production_scan_x, production_scan_x,
                                           1, 2560, 2400, 2900, production_materials};

std::span<const ProfileStation> footprint(const Material& item);

struct Profile {
    unsigned rgb_width{96}, hsi_width{80}, hsi_bands{16}, depth_width{64}, packet_lines{2};
};

struct Scan {
    std::vector<std::byte> bytes;
    std::size_t data_bytes{};
    std::optional<m01::ByteRange> mask;
};

Scan scan(m01::Source source, const Profile& profile, const Layout& layout,
          double displacement, bool mask_depth, double sensor_x);
std::vector<std::string> wavelengths(unsigned bands);
std::vector<int> preview(const m01::Observation& observation, const Profile& profile);

}  // namespace waste::m01sim
