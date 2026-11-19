#include "../device_source.h"

namespace waste::m01 {

bool valid_e05(const PacketView& packet) {
    return packet.source == Source::E05 && !packet.format.coordinates.empty() &&
           !packet.format.units.empty() && valid_packet_layout(packet);
}

}  // namespace waste::m01
