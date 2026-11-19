#include "../device_source.h"

namespace waste::m01 {

bool valid_rgb(const PacketView& packet) {
    return packet.source == Source::Rgb && valid_packet_layout(packet);
}

}  // namespace waste::m01
