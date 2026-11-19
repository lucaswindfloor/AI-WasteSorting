#include "../device_source.h"

namespace waste::m01 {

bool valid_sync(const SyncInput& input, const Config& config) {
    const auto& event = input.event;
    return event.key.sync_session == config.sync.session &&
           event.key.encoder_epoch == config.encoder_epoch &&
           (event.direction >= -1 && event.direction <= 1) &&
           (!event.source_time || !event.source_time->clock_domain.empty());
}

}  // namespace waste::m01
