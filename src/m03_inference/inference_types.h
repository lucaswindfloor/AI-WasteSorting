#pragma once

#include "m02_calibration/calibration_types.h"

#include <algorithm>
#include <compare>
#include <functional>

namespace waste::m03 {

using Tick = m01::Tick;
using Clock = std::function<Tick()>;

inline Tick current_tick(const Clock& clock, Tick floor) {
    return clock ? std::max(floor, clock()) : floor;
}

enum class Route { Rgb, Hsi, Joint };
enum class State { Completed, NoDetection, InputInvalid, Expired, QueueFull, Failed, Retired, Rejected };

struct Generation {
    std::string run_id;
    std::uint64_t config_epoch{};
    auto operator<=>(const Generation&) const = default;
};

struct ModelKey {
    std::string package, version;
    auto operator<=>(const ModelKey&) const = default;
};

struct ModelIdentity {
    ModelKey key;
    std::string artifact_digest, backend, precision, processing_version;
    auto operator<=>(const ModelIdentity&) const = default;
};

struct WindowId {
    Generation generation;
    ModelKey model;
    // Inclusive bounds in the shared trigger stream.
    std::uint64_t first{}, last{};
};

struct InputRow {
    std::shared_ptr<const m02::Result> owner;
    std::size_t trigger_index{};
    Tick received{}, valid_until{};
};

struct Window {
    WindowId id;
    std::vector<std::optional<InputRow>> rows;
    Tick first_received{}, ready{}, valid_until{};
};

struct SourceLocation {
    std::uint64_t segment_sequence{};
    m01::TriggerKey trigger;
    m01::ObservationId observation;
    m01::ByteRange source_bytes;
    std::size_t column{};
    auto operator<=>(const SourceLocation&) const = default;
};

struct SampleLocation {
    SourceLocation source;
    m02::ProjectedPoint belt;
    m02::TimeEstimate time;
    std::optional<m02::CountEstimate> count;
    std::optional<m02::BeltShift> applied_shift;
    std::optional<double> assumed_height_mm;
};

struct Tensor {
    m01::Source source;
    std::size_t height{}, width{}, channels{};
    // Contiguous floats and a validity byte for each value, in HWC order.
    std::vector<float> values;
    std::vector<std::uint8_t> valid;
};

struct PixelMap {
    std::optional<SampleLocation> rgb, hsi;
};

struct PreparedInput {
    std::size_t height{}, width{};
    std::vector<Tensor> tensors;
    std::vector<PixelMap> source_map;
    std::size_t retained_bytes{};
};

enum class EvidenceKind { Contour, Material, JointInstance };
enum class ScoreKind { Probability, Score };

struct Evidence {
    std::size_t instance{};
    std::string label;
    EvidenceKind kind{};
    ScoreKind score_kind{};
    float score{};
    std::vector<SampleLocation> samples;
};

struct Result {
    WindowId window;
    ModelIdentity model;
    std::optional<m02::Versions> calibration;
    State state;
    std::string reason;
    Tick deadline{}, first_received{}, ready{}, finished{};
    std::optional<Tick> prepared, started, backend_completed;
    std::vector<Evidence> evidence;
};

struct Report {
    std::vector<Result> windows;
    std::vector<m01::Diagnostic> acquisition_diagnostics;
};

}  // namespace waste::m03
