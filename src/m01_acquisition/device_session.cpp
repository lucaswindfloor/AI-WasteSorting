#include "device_session.h"
#include "device_source.h"

#include <stdexcept>

namespace waste::m01 {

const SourceConfig* source_config(const Config& config, Source source) {
    switch (source) {
    case Source::Sync: return &config.sync;
    case Source::Rgb: return &config.rgb;
    case Source::Hsi: return &config.hsi;
    case Source::E05: return config.e05 ? &*config.e05 : nullptr;
    }
    return nullptr;
}

bool valid_packet_layout(const PacketView& packet) {
    const auto within = [&](ByteRange range) {
        return range.size && range.offset <= packet.payload.size() &&
               range.size <= packet.payload.size() - range.offset;
    };
    if (packet.payload.empty() || packet.observations.empty() ||
        packet.format.encoding.empty() || packet.format.transport_layout.empty() ||
        !packet.format.width || !packet.format.rows) {
        return false;
    }
    for (const auto& row : packet.observations) {
        if (row.id.source != packet.source || row.id.session != packet.session ||
            row.id.row >= packet.format.rows || !within(row.bytes) ||
            (row.valid_mask && !within(*row.valid_mask)) ||
            (row.association_verified && !row.trigger) ||
            (row.source_time && row.source_time->clock_domain.empty()) ||
            row.encoder_count.has_value() != row.encoder_epoch.has_value()) {
            return false;
        }
    }
    return true;
}

void validate_config(const Config& config) {
    const auto verified = [](const SourceConfig& source) {
        return source.settings_verified && !source.device_id.empty();
    };
    if (config.run_id.empty() || !verified(config.sync) || !verified(config.rgb) ||
        !verified(config.hsi) || (config.e05 && !verified(*config.e05))) {
        throw std::invalid_argument("missing run identity or verified device settings");
    }
    if (config.first_expected_trigger && !config.contiguous_trigger_ids) {
        throw std::invalid_argument("a trigger range requires contiguous IDs");
    }
    if (config.source_stale_timeout <= Tick::zero() || config.match_timeout <= Tick::zero() || config.segment_timeout <= Tick::zero() ||
        !config.max_pending || !config.recent_history || !config.max_segment_items ||
        !config.max_segment_bytes || !config.max_inflight_segments || !config.max_packet_bytes || !config.max_packet_items ||
        !config.buffer_high_water || config.buffer_high_water > config.buffer_capacity ||
        config.max_packet_bytes > config.buffer_capacity) {
        throw std::invalid_argument("invalid acquisition limits");
    }
}

}  // namespace waste::m01
