#pragma once

#include "calibration_types.h"

namespace waste::m02 {

bool finite(Point point);
bool valid_bounds(const Bounds& bounds);
bool contains(const Bounds& bounds, Point point);
bool valid_transform(const RigidTransform& transform);
Point apply(const RigidTransform& transform, Point point);
RigidTransform compose(const RigidTransform& outer, const RigidTransform& inner);
RigidTransform inverse(const RigidTransform& transform);

enum class Frame { Belt, Depth, RobotBase };
enum class PoseTarget { Tcp, Flange };

std::optional<ProjectedPoint> transform_point(const Snapshot& snapshot, Frame from,
                                               Frame to, Point point);
std::optional<RigidTransform> robot_pose(const Snapshot& snapshot,
                                         const RigidTransform& belt_from_tcp,
                                         std::string_view tool_id, PoseTarget target);

}  // namespace waste::m02
