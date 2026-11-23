#pragma once

#include "m02_calibration/calibration_service.h"
#include "simulator.h"

namespace waste::m01sim {

inline constexpr std::string_view exposure_basis = "simulated sensor exposure";
inline constexpr std::string_view calibration_settings = "sim-exposure";

m01::Tick clock_offset(m01::Source source);
m01::Tick hsi_exposure_delay(Scenario scenario, m01::Tick interval);
m01::Tick hsi_transport_delay(Scenario scenario);
double hsi_scan_position(const Layout& layout, Scenario scenario, m01::Tick interval);
bool hsi_has_native_count(Scenario scenario, unsigned trigger);
m02::Snapshot calibration(const m01::Config& config, const Layout& layout, Scenario scenario, m01::Tick interval);
std::string_view name(m02::CoverageStatus status);

}  // namespace waste::m01sim
