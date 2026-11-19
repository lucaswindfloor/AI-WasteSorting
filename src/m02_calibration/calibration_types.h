#pragma once

#include "m01_acquisition/acquisition_types.h"

#include <array>
#include <string_view>
#include <utility>

namespace waste::m02 {

using m01::Quality;

struct Point {
    double x{}, y{}, z{};
};

struct Bounds {
    Point min, max;
};

struct RigidTransform {
    // Row-major rotation. Translation in millimeters.
    std::array<double, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    Point translation;
};

struct Versions {
    std::string geometry, clock, encoder, spectral_reference;
};

struct DeviceKey {
    m01::Source source;
    std::string device_id, firmware, sdk;
    std::uint64_t session{};
    m01::DataFormat format;
};

struct ClockMapping {
    m01::Source source;
    std::uint64_t session{};
    std::string domain, time_basis;
    std::int64_t source_anchor{}, first_tick{}, last_tick{};
    m01::Tick host_anchor{};
    double ns_per_tick{};
    m01::Tick error{};
};

struct TriggerDelay {
    m01::Source source;
    std::uint64_t session{};
    m01::Tick offset{}, error{};
};

enum class TimeBasis { SourceClock, TriggerDelay };

struct TimeEstimate {
    m01::Tick value{}, error{};
    TimeBasis basis{TimeBasis::SourceClock};
};

struct EncoderConfig {
    std::uint64_t epoch{};
    // Positive value means increasing counts move material toward positive B.x.
    double signed_mm_per_count{};
    double lock_error_counts{}, native_error_counts{}, interpolation_error_counts{};
    m01::Tick max_sample_gap{};
    double max_count_rate_per_second{};
    double max_displacement_mm{};
    std::vector<m01::Source> verified_native_sources;
};

struct CountSample {
    std::uint64_t epoch{};
    std::int64_t count{};
    TimeEstimate time;
    int direction{};
};

enum class CountBasis { TriggerLatch, SourceExposureLatch, Interpolation };

struct CountEstimate {
    std::uint64_t epoch{};
    long double value{};
    double error_counts{};
    CountBasis basis{CountBasis::TriggerLatch};
};

struct BeltShift {
    double mm{}, error_mm{};
    CountEstimate from, to;
};

struct ScanLayer {
    double height_mm{};
    std::vector<Point> columns;
    std::vector<std::uint8_t> valid_columns{};
};

struct ScanCalibration {
    std::vector<ScanLayer> layers;
    double residual_mm{};
};

struct DepthCalibration {
    RigidTransform to_belt;
    Bounds source_bounds;
    Bounds belt_bounds;
    double residual_mm{};
    // Used for HEIGHT_F32LE. XYZ_F32LE already carries all three coordinates.
    Point grid_origin;
    double row_step_mm{}, column_step_mm{};
};

struct RobotCalibration {
    RigidTransform base_from_belt;
    Bounds belt_bounds;
    double residual_mm{};
    std::string tool_id;
    RigidTransform flange_from_tcp;
};

enum class SpectralLevel { Raw, DarkWhiteCorrected };
enum class ReferenceLayout { PerBand, PerColumnBand };

struct SpectralCalibration {
    std::string settings_id;
    SpectralLevel input_level{SpectralLevel::Raw};
    ReferenceLayout reference_layout{ReferenceLayout::PerBand};
    std::vector<std::string> bands;
    std::vector<double> dark, white;
    double denominator_min{}, saturation{};
    double temperature_min{}, temperature_max{}, light_min{}, light_max{};
    m01::Tick environment_max_age{};
};

struct Environment {
    std::string run_id;
    std::uint64_t generation{};
    std::string settings_id;
    m01::Tick measured{};
    double temperature{}, light{};
    bool reference_healthy{};
};

struct Snapshot {
    std::string run_id;
    std::uint64_t generation{};
    Versions versions;
    std::vector<DeviceKey> devices;
    std::vector<ClockMapping> clocks;
    std::vector<TriggerDelay> trigger_delays;
    EncoderConfig encoder;
    ScanCalibration rgb, hsi;
    double reference_height_mm{};
    std::optional<DepthCalibration> depth;
    std::optional<RobotCalibration> robot;
    std::optional<SpectralCalibration> spectral;
    m01::Tick max_exposure_separation{}, max_reference_separation{};
    double max_pair_dx_mm{}, max_pair_dy_mm{};
};

struct ProjectedPoint {
    Quality quality{Quality::Missing};
    Point position;
    double error_mm{};
};

enum class SpectralIssue { None, Masked, NonFinite, Saturated, InvalidReference };

struct SpectralRow {
    SpectralLevel level{SpectralLevel::Raw};
    std::string reference_version;
    std::vector<double> samples;
    std::vector<Quality> quality;
    std::vector<SpectralIssue> sample_issues;
    std::string_view reason;
};

struct AlignedObservation {
    m01::Observation source;
    Quality source_quality{Quality::Unknown};
    std::optional<TimeEstimate> time;
    std::optional<CountEstimate> count;
    std::optional<TimeEstimate> reference_time;
    std::optional<BeltShift> shift;
    // Geometry in B at measurement time, before belt motion.
    std::vector<ProjectedPoint> measured;
    // Empty if there is no verified count relationship to the selected reference.
    std::vector<ProjectedPoint> aligned;
    std::optional<double> assumed_height_mm;
    std::optional<SpectralRow> spectrum;
    std::string_view issue;
};

struct Coverage {
    std::size_t rgb_column{}, hsi_column{};
};

struct AlignedTrigger {
    m01::TriggerRecord source;
    std::optional<TimeEstimate> reference_time;
    CountEstimate reference_count;
    std::optional<AlignedObservation> rgb, hsi;
    std::vector<Coverage> common_coverage;
};

struct Result {
    std::shared_ptr<const m01::Segment> source;
    std::shared_ptr<const Snapshot> snapshot;
    std::vector<AlignedTrigger> triggers;
    std::vector<AlignedObservation> independent;
    std::vector<m01::Diagnostic> acquisition_diagnostics;
    std::string_view issue;
};

}  // namespace waste::m02
