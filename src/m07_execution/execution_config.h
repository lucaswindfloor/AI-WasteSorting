#pragma once

#include "execution_types.h"

namespace waste::m07 {

struct Lane {
    std::string station, robot, controller, terminal;
    m06::Contract contract{};
    m06::TrackingMode tracking{};
    Tick send_to_receipt_upper{};
    // Required for A. The installed PREPARE endpoint must be planning-only.
    bool prepare_is_planning_only{};
};

struct ExecutionConfig {
    m03::Generation generation;
    std::string version, writer_session, process_source, safety_source;
    Tick valid_until{}, max_field_age{}, max_event_age{}, query_interval{};
    std::size_t max_attempts{}, max_events_per_attempt{}, max_wire_bytes{}, max_resources{}, max_intervals{};
    std::vector<Lane> lanes;
};

void validate(const ExecutionConfig& config);
const Lane* lane_for(const ExecutionConfig& config, std::string_view station);
const DeviceLink* current_link(std::span<const DeviceLink> links, std::string_view id, Tick now,
                               Tick max_age);
bool gate_open(const ExecutionConfig& config, const FieldState* field, Tick now);
bool valid_field(const ExecutionConfig& config, const FieldState& field, Tick now);

}  // namespace waste::m07
