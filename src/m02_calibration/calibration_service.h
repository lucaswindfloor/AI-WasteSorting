#pragma once

#include "calibration_snapshot.h"
#include "coordinate_transform.h"

namespace waste::m02 {

struct SyncWindow {
    std::string_view run_id;
    std::uint64_t generation{};
    std::span<const m01::TriggerEvent> events;
};

struct ObservationReference {
    m01::ObservationId observation;
    m01::TriggerEvent reference;
};

struct ProcessContext {
    const SyncWindow* sync_window{};
    const Environment* environment{};
    // Optional independent stream alignment to an explicitly selected sync event.
    const m01::TriggerEvent* independent_reference{};
    std::span<const m01::Diagnostic> acquisition_diagnostics;
    // A binding applies only to its named observation and takes precedence over the default.
    std::span<const ObservationReference> independent_references{};
};

struct CoordinateReference {
    std::string run_id;
    std::uint64_t generation{};
    std::string geometry_version;
    m01::Tick time{};
    CountEstimate count;
};

struct PointRequest {
    CoordinateReference reference;
    Frame from{Frame::Belt}, to{Frame::RobotBase};
    Point point;
};

struct PoseRequest {
    CoordinateReference reference;
    std::string tool_id;
    RigidTransform belt_from_tcp;
    PoseTarget target{PoseTarget::Tcp};
};

enum class TransformIssue {
    None, UnavailableGeneration, GeometryVersionMismatch, InvalidReference,
    ToolMismatch, UnsupportedFrameOrOutOfDomain
};

template<class Value>
struct TransformResult {
    std::shared_ptr<const Snapshot> snapshot;
    CoordinateReference reference;
    TransformIssue issue{TransformIssue::None};
    std::optional<Value> value;
    double calibration_residual_mm{};
};

class CalibrationService {
public:
    void install(Snapshot snapshot);
    void retire(const std::string& run_id, std::uint64_t generation);
    Result process(std::shared_ptr<const m01::Segment> segment, const ProcessContext& context = {}) const;
    TransformResult<ProjectedPoint> transform_point(const PointRequest& request) const;
    TransformResult<RigidTransform> transform_pose(const PoseRequest& request) const;

private:
    CalibrationSnapshots snapshots_;
};

}  // namespace waste::m02
