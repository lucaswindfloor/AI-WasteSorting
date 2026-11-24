#include "scene.h"
#include "material_texture.h"

#include <algorithm>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace waste::m01sim {

std::span<const ProfileStation> footprint(const Material& item) {
    // Stations run from the trailing end to the leading end of the object.
    static constexpr std::array pet{
        ProfileStation{0.00, 0.68}, ProfileStation{0.04, 0.91}, ProfileStation{0.09, 1.00},
        ProfileStation{0.69, 1.00}, ProfileStation{0.75, 0.93}, ProfileStation{0.85, 0.44},
        ProfileStation{0.91, 0.44}, ProfileStation{0.92, 0.57}, ProfileStation{0.99, 0.57},
        ProfileStation{1.00, 0.50},
    };
    static constexpr std::array can{
        ProfileStation{0.00, 0.76}, ProfileStation{0.04, 0.95}, ProfileStation{0.09, 1.00},
        ProfileStation{0.91, 1.00}, ProfileStation{0.96, 0.95}, ProfileStation{1.00, 0.76},
    };
    if (item.kind == "PET") return pet;
    return can;
}

namespace {

struct Cut {
    int index{-1};
    double height{}, along{}, across{};
};

Cut sample(const Layout& layout, double sensor_x, double displacement, double lateral) {
    for (std::size_t i = 0; i < layout.objects.size(); ++i) {
        const auto& item = layout.objects[i];
        const auto x = sensor_x - item.x - displacement;
        const auto y = lateral - item.y;
        if (std::abs(x) >= item.length / 2) continue;
        const auto along = x / item.length + 0.5;
        const auto profile = footprint(item);
        const auto right = std::max(profile.begin() + 1,
            std::ranges::lower_bound(profile, along, {}, &ProfileStation::along));
        const auto left = right - 1;
        const auto fraction = (along - left->along) / (right->along - left->along);
        const auto width = item.width * std::lerp(left->width_fraction, right->width_fraction, fraction);
        const auto edge = (2 * y / width);
        if (std::abs(edge) >= 1) continue;
        return {static_cast<int>(i), item.height * std::sqrt(1 - edge * edge), along, (edge + 1) / 2};
    }
    return {};
}

void pack(std::vector<std::byte>& bytes, std::uint32_t value, unsigned length) {
    for (unsigned i = 0; i < length; ++i) bytes.push_back(std::byte((value >> (i * 8)) & 255));
}

std::uint32_t unpack(std::span<const std::byte> bytes, std::size_t offset, unsigned length) {
    std::uint32_t number = 0;
    for (unsigned i = 0; i < length; ++i) number |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8 * i);
    return number;
}

unsigned width_for(m01::Source source, const Profile& profile) {
    if (source == m01::Source::Rgb) return profile.rgb_width;
    if (source == m01::Source::Hsi) return profile.hsi_width;
    return profile.depth_width;
}

}  // namespace

double surface_height(const Layout& layout, double belt_x, double belt_y, double displacement) {
    return sample(layout, belt_x, displacement, belt_y).height;
}

std::vector<std::string> wavelengths(unsigned bands) {
    std::vector<std::string> values;
    for (unsigned band = 0; band < bands; ++band) {
        values.push_back(std::to_string(950 + band * 700 / (bands - 1)));
    }
    return values;
}

Scan scan(m01::Source source, const Profile& profile, const Layout& layout,
          double displacement, bool mask_depth, double sensor_x) {
    Scan out;
    const auto width = width_for(source, profile);
    const auto item_bytes = source == m01::Source::Hsi ? 2 * profile.hsi_bands : source == m01::Source::Rgb ? 3 : 4;
    out.bytes.reserve(width * (item_bytes + (mask_depth ? 1 : 0)));
    for (unsigned i = 0; i < width; ++i) {
        const auto cut = sample(layout, sensor_x, displacement, (i + 0.5) * layout.width / width);
        const auto stripe = i % 9;
        if (source == m01::Source::Rgb) {
            const std::array palettes{std::array{22u, 34u, 47u}, std::array{41u, 189u, 229u},
                                      std::array{227u, 139u, 57u}};
            auto rgb = palettes[cut.index + 1];
            if (layout.length && cut.index >= 0) {
                const auto tx = std::min(static_cast<unsigned>(cut.along * material_texture_width), material_texture_width - 1);
                const auto ty = std::min(static_cast<unsigned>(cut.across * material_texture_height), material_texture_height - 1);
                const auto color = material_textures[cut.index][ty * material_texture_width + tx];
                rgb = {color >> 16, (color >> 8) & 255u, color & 255u};
            }
            for (auto channel : rgb) pack(out.bytes, std::min(255u, channel + stripe), 1);
        } else if (source == m01::Source::Hsi) {
            for (unsigned band = 0; band < profile.hsi_bands; ++band) {
                const auto wave = band * 100 / (profile.hsi_bands - 1);
                const unsigned value = cut.index < 0 ? 280 + stripe * 22 :
                    cut.index == 0 ? 5400 + wave * 169 - (wave > 55 && wave < 78 ? 3500 : 0) :
                                     24000 - wave * 95 + stripe * 110;
                pack(out.bytes, value, 2);
            }
        } else {
            pack(out.bytes, std::bit_cast<std::uint32_t>(static_cast<float>(cut.height)), 4);
        }
    }
    out.data_bytes = out.bytes.size();
    if (source == m01::Source::E05 && mask_depth) {
        out.mask = m01::ByteRange{out.data_bytes, width};
        for (unsigned i = 0; i < width; ++i) pack(out.bytes, (i >= width / 3 && i < width / 2) ? 0 : 1, 1);
    }
    return out;
}

std::vector<int> preview(const m01::Observation& observation, const Profile& profile) {
    const auto& header = observation.header;
    const auto payload = std::span(*observation.payload);
    const auto source = header.id.source;
    const auto width = width_for(source, profile);
    std::vector<int> pixels;
    for (unsigned i = 0; i < preview_width; ++i) {
        const auto column = (2 * i + 1) * width / (2 * preview_width);
        const auto base = header.bytes.offset;
        if (source == m01::Source::Rgb) {
            for (unsigned channel = 0; channel < 3; ++channel) {
                pixels.push_back(unpack(payload, base + column * 3 + channel, 1));
            }
        } else if (source == m01::Source::Hsi) {
            const auto band = profile.hsi_bands / 2;
            pixels.push_back(unpack(payload, base + (column * profile.hsi_bands + band) * 2, 2));
        } else if (header.valid_mask && payload[header.valid_mask->offset + column] == std::byte{0}) {
            pixels.push_back(-1);
        } else {
            const auto height = std::bit_cast<float>(unpack(payload, base + column * 4, 4));
            pixels.push_back(static_cast<int>(std::lround(height * 10)));
        }
    }
    return pixels;
}

}  // namespace waste::m01sim
