#pragma once

#include "scheduling_types.h"

namespace waste::m06 {

struct ContactRegion {
    std::string id;
    m04::Point min, max;
};

struct Budget {
    Tick handoff_to_receipt_min{}, handoff_to_receipt_max{};
    Tick receipt_to_contact_min{}, receipt_to_contact_max{};
    Tick early_margin{}, late_margin{};
};

struct Lane {
    std::string station, robot;
    Contract contract{Contract::B};
    TrackingMode tracking{TrackingMode::ControllerNative};
    std::vector<ContactRegion> contact_regions;
    Budget budget;
    // Occupancy starts/ends at offsets from the planned handoff instant.
    Interval robot_use, tool_use, bin_use;
};

struct SchedulingConfig {
    m03::Generation generation;
    std::string version, scheduler_session;
    Tick valid_until{}, max_prediction_age{}, transaction_lead{};
    double max_tracking_error_mm{}, forecast_extra_error_mm{};
    std::size_t max_batch{}, max_attempts{}, max_intervals{}, max_resources{}, max_retries{};
    std::vector<Lane> lanes;
    bool enable_e11{};
};

void validate(const SchedulingConfig& config);
const Lane* find_lane(const SchedulingConfig& config, std::string_view station);

}  // namespace waste::m06
