#pragma once

#include "m04_tracking/tracking_types.h"

namespace waste::m05 {

using Tick = m04::Tick;

enum class State { Ready, Pending, NeedsRemeasurement, Rejected, NotEvaluated };
enum class Reason {
    None, WrongGeneration, InvalidTarget, AmbiguousTarget, TargetExpired,
    MaterialUnknown, MaterialConflict, MaterialExpired, MaterialExcluded,
    ToolNotReported, ToolStale, ToolMismatch, ToolChanging, ToolUnavailable,
    BinNotReported, BinStale, BinIdentityMismatch, BinAbsent, BinFull, BinChanging,
    BinUnavailable, FallbackNotAuthorized, NoBin, SnapshotVersionChanged, SnapshotInvalid,
    InvalidSurface, SurfaceMissing, SurfaceExpired, ContactOutside, NeighborOverlap,
    HeightVariation, NormalMissing, TiltExceeded, CenterMissing, Capacity,
    TargetVersionChanged, RecipeChanged, CandidateExpired, RemeasurementRejected
};

struct Check {
    State state{State::NotEvaluated};
    Reason reason{Reason::None};
    Tick valid_until{};
};

struct ToolIdentity {
    std::string station, tool, configuration, tcp;
    auto operator<=>(const ToolIdentity&) const = default;
};

struct BinIdentity {
    std::string slot, physical_bin;
    auto operator<=>(const BinIdentity&) const = default;
};

struct TargetBinding {
    m03::Generation generation;
    m04::TargetId id{};
    std::uint64_t version{};
    m04::Reference reference;
    bool operator==(const TargetBinding&) const = default;
};

inline TargetBinding binding(const m04::Target& target) {
    return {target.generation, target.id, target.version, target.footprint.reference};
}

struct FactTime {
    std::string source;
    Tick observed{}, valid_until{};
    bool operator==(const FactTime&) const = default;
};

enum class ToolCondition { Ready, Changing, Unavailable, Fault, Unknown };
enum class BinCondition { Ready, Absent, Full, Changing, Unavailable, Unknown };

struct ToolFact {
    ToolIdentity identity;
    ToolCondition condition{ToolCondition::Unknown};
    FactTime time;
    bool operator==(const ToolFact&) const = default;
};

struct BinFact {
    BinIdentity identity;
    // Increment when the physical bin is removed/replaced or confirmation changes.
    std::uint64_t placement_revision{};
    BinCondition condition{BinCondition::Unknown};
    FactTime time;
    bool operator==(const BinFact&) const = default;
};

struct Snapshot {
    m03::Generation generation;
    std::uint64_t revision{};
    std::string bin_mapping_version;
    std::vector<ToolFact> tools;
    std::vector<BinFact> bins;
};

enum class CellState { Observed, Missing, OutsideTarget, Neighbor };

struct Cell {
    CellState state{CellState::Missing};
    std::optional<m04::SampleKey> source{};
    std::optional<double> tilt_upper_deg{};
};

struct SurfaceGrid {
    TargetBinding target;
    std::string proof_id, profile_version;
    // B.x phase (along belt), B.y across belt, in millimeters.
    double x0{}, y0{};
    std::size_t rows{}, columns{};
    // Row-major. Rows advance along B.x, columns along B.y.
    std::vector<Cell> cells;
    Tick valid_until{};
};

struct Region {
    std::string id;
    double x{}, y{};
    std::size_t grid_index{};
};

struct Observation {
    std::span<const SurfaceGrid> grids;
    std::span<const Region> regions;
};

enum class MissingComponent { Coverage, CenterHeight, Normal };

struct Contact {
    Check check;
    std::vector<Reason> reasons;
    std::vector<MissingComponent> missing;
    std::optional<m04::Point> center;
    double required_radius_mm{}, height_delta_mm{};
    std::optional<double> tilt_upper_deg;
    std::vector<m04::SourceRef> sources;
};

struct ToolCheck {
    ToolIdentity expected;
    std::optional<ToolFact> observed;
    Check check;
};

struct BinCheck {
    BinIdentity expected;
    std::optional<BinFact> observed;
    Check check;
};

struct RegionCheck {
    std::string region_id, proof_id;
    ToolIdentity tool;
    Contact contact;
};

struct RemeasurementReference {
    TargetBinding requested;
    std::string appointment_id;
    Tick measured{}, valid_until{};
};

struct Candidate {
    TargetBinding target;
    std::string recipe_version, profile_version, bin_mapping_version, region_id, proof_id;
    std::uint64_t snapshot_revision{};
    ToolFact tool;
    BinFact bin;
    std::string rule_label;
    Contact contact;
    Tick assessed{}, valid_until{};
    std::vector<BinFact> route_dependencies{};
    std::optional<RemeasurementReference> remeasurement{};
};

struct Remeasurement {
    TargetBinding target;
    std::string recipe_version, profile_version, bin_mapping_version, region_id, proof_id;
    std::uint64_t snapshot_revision{};
    ToolFact tool;
    BinFact bin;
    std::string rule_label;
    double x{}, y{}, required_radius_mm{};
    std::vector<MissingComponent> required;
    Tick assessed{}, valid_until{};
};

struct Result {
    TargetBinding target;
    std::string recipe_version;
    std::uint64_t snapshot_revision{};
    Tick assessed{};
    State state{State::NotEvaluated};
    Reason reason{Reason::None};
    Check target_check, material_check, bin_route;
    std::string rule_label;
    std::vector<m04::SampleKey> material_sources, disputed_material;
    std::vector<ToolCheck> tools;
    std::vector<BinCheck> bins;
    std::vector<RegionCheck> regions;
    std::vector<Candidate> candidates;
    std::vector<Remeasurement> remeasurements;
};

}  // namespace waste::m05
