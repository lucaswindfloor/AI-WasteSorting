#include "scene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace waste::m01sim {
namespace {

struct Cut {
    int index{-1};
    double height{};
};

Cut sample(double displacement, double lateral) {
    for (std::size_t i = 0; i < materials.size(); ++i) {
        const auto& item = materials[i];
        const auto x = scan_x - item.x - displacement;
        const auto y = lateral - item.y;
        if (std::abs(x) >= item.length / 2) continue;
        auto width = item.width;
        if (item.kind == "PET" && x > 0.27 * item.length) {
            width *= x > 0.37 * item.length ? 0.44 : 0.72;
        }
        const auto edge = (2 * y / width);
        if (std::abs(edge) >= 1) continue;
        return {static_cast<int>(i), item.height * std::sqrt(1 - edge * edge)};
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

std::vector<std::string> wavelengths(unsigned bands) {
    std::vector<std::string> values;
    for (unsigned band = 0; band < bands; ++band) {
        values.push_back(std::to_string(950 + band * 700 / (bands - 1)));
    }
    return values;
}

Scan scan(m01::Source source, const Profile& profile, double displacement, bool mask_depth) {
    Scan out;
    const auto width = width_for(source, profile);
    for (unsigned i = 0; i < width; ++i) {
        const auto cut = sample(displacement, (i + 0.5) * belt_width / width);
        const auto stripe = i % 9;
        if (source == m01::Source::Rgb) {
            const std::array palettes{std::array{22u, 34u, 47u}, std::array{41u, 189u, 229u},
                                      std::array{227u, 139u, 57u}};
            auto rgb = palettes[cut.index + 1];
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
        const auto column = i * width / preview_width;
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
