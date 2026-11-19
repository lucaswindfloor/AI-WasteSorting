#pragma once

#include "scheduling_config.h"

namespace waste::m06 {

struct ContactComputation {
    std::optional<ContactWindow> window;
    Reason reason{Reason::None};
};

ContactComputation contact_window(const SchedulingConfig& config, const m05::Candidate& candidate,
                                  const m04::Prediction& prediction, const MotionForecast& forecast,
                                  const ContactRegion& region, Tick now);

}  // namespace waste::m06
