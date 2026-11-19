#include "../device_source.h"

#include <algorithm>

namespace waste::m01 {

bool valid_hsi(const PacketView& packet) {
    return packet.source == Source::Hsi && !packet.format.bands.empty() &&
           std::ranges::none_of(packet.format.bands, &std::string::empty) &&
           valid_packet_layout(packet);
}

}  // namespace waste::m01
