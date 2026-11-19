#pragma once

#include "m03_inference/inference_types.h"

namespace waste::m04 {

using Tick = m01::Tick;
using TargetId = std::uint64_t;

struct Point {
    double x{}, y{}, z{};
    bool operator==(const Point&) const = default;
};

struct Reference {
    Tick time{}, time_error{};
    std::uint64_t encoder_epoch{};
    long double count{};
    double count_error{};
    bool operator==(const Reference&) const = default;
};

struct SampleKey {
    m01::ObservationId observation;
    std::size_t column{};
    auto operator<=>(const SampleKey&) const = default;
};

struct SourceRef {
    SampleKey key;
    std::uint64_t segment{};
    m01::ByteRange bytes;
    bool operator==(const SourceRef&) const = default;
};

struct Sample {
    SourceRef source;
    Reference reference;
    Point position;
    double error_mm{};
    // Preserve the coordinate reference delivered by M02/M03 after re-anchoring.
    std::optional<Reference> source_reference{};
    std::optional<double> upstream_shift_mm{};
    bool operator==(const Sample&) const = default;
};

struct Footprint {
    Reference reference;
    std::vector<Sample> points;
    Point center;
    // Bounding range of the observed points. This does not imply a filled mask.
    Point min, max;
    double error_mm{};
    bool operator==(const Footprint&) const = default;
};

struct WindowKey {
    m03::ModelKey model;
    std::uint64_t first{}, last{};
    auto operator<=>(const WindowKey&) const = default;
};

struct Origin {
    WindowKey window;
    std::size_t instance{};
    auto operator<=>(const Origin&) const = default;
};

enum class TargetState { Tracked, Ambiguous, Invalid };
enum class MaterialStatus { Known, Insufficient, Conflict, AmbiguousOwnership };

struct Material {
    MaterialStatus status{MaterialStatus::Insufficient};
    std::string label;
    std::vector<SampleKey> supporting, disputed;
    std::optional<Tick> valid_until;
    bool operator==(const Material&) const = default;
};

struct SurfacePatch {
    // Contiguous measured columns from one E05 row in the target reference.
    // No fill, extrapolation or tool suitability is implied between rows.
    std::vector<Sample> points;
    Point min, max;
    Tick valid_until{};
    bool operator==(const SurfacePatch&) const = default;
};

enum class Reason {
    None, Unconfigured, WrongGeneration, InputExpired, InvalidInput, Duplicate,
    InsufficientContour, AssociationAmbiguous, Capacity, SurfaceMissing,
    SurfaceUnassigned, MaterialUnassigned, TargetMissing, VersionMismatch, EncoderUnavailable,
    EncoderStale, EncoderInvalid, EncoderReset, MotionUnverified, OutsideRange,
    TargetExpired, ConfigurationRetired
};

struct Target {
    TargetId id{};
    std::uint64_t version{};
    m03::Generation generation;
    TargetState state{TargetState::Tracked};
    Reason reason{Reason::None};
    Footprint footprint;
    Material material;
    std::vector<SurfacePatch> surfaces;
    Tick valid_until{}, association_time{};
};

struct EncoderState {
    m03::Generation generation;
    Reference reference;
    std::string encoder_version;
    // Direction in B (+x or -x); zero means stationary.
    int direction{};
    bool valid{}, following_verified{};
    bool operator==(const EncoderState&) const = default;
};

struct Prediction {
    TargetId target{};
    std::uint64_t version{};
    m03::Generation generation;
    Reason reason{Reason::None};
    Reference origin, current;
    Point position;
    double applied_mm{}, error_mm{};
    Tick valid_until{};
};

struct AssociationEvent {
    Origin origin;
    std::optional<TargetId> matched;
    std::vector<TargetId> candidates;
    Reason reason{Reason::None};
};

struct EvidenceEvent {
    std::optional<Origin> origin;
    std::optional<SourceRef> source;
    Reason reason{Reason::None};
};

struct Report {
    std::vector<Target> targets;
    std::vector<AssociationEvent> associations;
    std::vector<EvidenceEvent> evidence;
};

}  // namespace waste::m04
