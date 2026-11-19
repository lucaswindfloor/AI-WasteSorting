#pragma once

#include "acquisition_types.h"

namespace waste::m01 {

// Vendor glue normalizes SDK metadata into these views and calls the service
// on its serialized ingress lane. Views need to live only through that call.
struct SyncInput {
    std::uint64_t generation{};
    TriggerEvent event;
    std::optional<Tick> host_received;
};

struct PacketView {
    std::uint64_t generation{};
    Source source;
    std::uint64_t session{};
    DataFormat format;
    std::span<const std::byte> payload;
    std::span<const ObservationHeader> observations;
    std::optional<Tick> host_received;
};

const SourceConfig* source_config(const Config& config, Source source);
bool valid_sync(const SyncInput& input, const Config& config);
bool valid_rgb(const PacketView& packet);
bool valid_hsi(const PacketView& packet);
bool valid_e05(const PacketView& packet);
bool valid_packet_layout(const PacketView& packet);

}  // namespace waste::m01
