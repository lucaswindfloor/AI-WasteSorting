#pragma once

#include "scene.h"
#include <filesystem>

namespace waste::m01sim {

enum class Scenario {
    Normal, Missing, Late, Mixed, SlowArchive, Reconnect, TransportDelay, ExposureDelay,
    Compensated, ExposureLimit, ClockMissing, CountInterpolation, InvalidWhite
};
enum class DepthMode { Independent, Encoder };

struct Options {
    Scenario scenario{Scenario::Mixed};
    DepthMode depth{DepthMode::Independent};
    unsigned speed_mm_s{1000};
    Profile profile;
    std::filesystem::path output;
};

struct Report {
    bool passed{};
    unsigned paired{}, missing{}, late{}, depth{}, verified{}, calibrated{}, covered{};
};

std::string_view name(Scenario scenario);
Report run(const Options& options);

}  // namespace waste::m01sim
