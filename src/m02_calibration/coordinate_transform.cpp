#include "coordinate_transform.h"

#include <cmath>

namespace waste::m02 {

bool finite(Point point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

bool valid_bounds(const Bounds& b) {
    return finite(b.min) && finite(b.max) &&
           b.min.x <= b.max.x && b.min.y <= b.max.y && b.min.z <= b.max.z;
}

bool contains(const Bounds& b, Point p) {
    return finite(p) && p.x >= b.min.x && p.x <= b.max.x &&
           p.y >= b.min.y && p.y <= b.max.y && p.z >= b.min.z && p.z <= b.max.z;
}

bool valid_transform(const RigidTransform& t) {
    constexpr double tolerance = 1e-6;
    if (!finite(t.translation)) {
        return false;
    }
    const auto& r = t.rotation;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            const auto dot = r[3 * i] * r[3 * j] + r[3 * i+1] * r[3 * j+1] + r[3 * i+2] * r[3 * j+2];
            if (!std::isfinite(dot) || std::abs(dot - (i == j ? 1.0 : 0.0)) > tolerance) {
                return false;
            }
        }
    }
    const auto determinant = r[0]*(r[4]*r[8]-r[5]*r[7]) -
                             r[1]*(r[3]*r[8]-r[5]*r[6]) + r[2]*(r[3]*r[7]-r[4]*r[6]);
    return std::abs(determinant - 1) <= tolerance;
}

Point apply(const RigidTransform& t, Point p) {
    const auto& r = t.rotation;
    return {r[0]*p.x+r[1]*p.y+r[2]*p.z+t.translation.x,
            r[3]*p.x+r[4]*p.y+r[5]*p.z+t.translation.y,
            r[6]*p.x+r[7]*p.y+r[8]*p.z+t.translation.z};
}

RigidTransform compose(const RigidTransform& outer, const RigidTransform& inner) {
    RigidTransform result;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            result.rotation[3 * i+j] = 0;
            for (std::size_t k = 0; k < 3; ++k) {
                result.rotation[3 * i+j] += outer.rotation[3 * i+k] * inner.rotation[3 * k+j];
            }
        }
    }
    result.translation = apply(outer, inner.translation);
    return result;
}

RigidTransform inverse(const RigidTransform& t) {
    RigidTransform result;
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            result.rotation[3 * i+j] = t.rotation[3 * j+i];
        }
    }
    result.translation = apply(result, {-t.translation.x, -t.translation.y, -t.translation.z});
    return result;
}

std::optional<ProjectedPoint> transform_point(const Snapshot& snapshot, Frame from,
                                              Frame to, Point point) {
    const auto supported = [](Frame frame) {
        return frame == Frame::Belt || frame == Frame::Depth || frame == Frame::RobotBase;
    };
    if (!finite(point) || !supported(from) || !supported(to)) {
        return {};
    }
    auto belt = point;
    double error = 0;
    const auto& depth = snapshot.depth;
    const auto& robot = snapshot.robot;
    if (from == Frame::Depth) {
        if (!depth || !contains(depth->source_bounds, point)) {
            return {};
        }
        belt = apply(depth->to_belt, point);
        if (!contains(depth->belt_bounds, belt)) {
            return {};
        }
        error = depth->residual_mm;
    } else if (from == Frame::RobotBase) {
        if (!robot) {
            return {};
        }
        belt = apply(inverse(robot->base_from_belt), point);
        if (!contains(robot->belt_bounds, belt)) {
            return {};
        }
        error = robot->residual_mm;
    }
    if (from == to) {
        return ProjectedPoint{Quality::Valid, point, 0};
    }
    auto projected = belt;
    if (to == Frame::Depth) {
        if (!depth || !contains(depth->belt_bounds, belt)) {
            return {};
        }
        projected = apply(inverse(depth->to_belt), belt);
        if (!contains(depth->source_bounds, projected)) {
            return {};
        }
        error += depth->residual_mm;
    } else if (to == Frame::RobotBase) {
        if (!robot || !contains(robot->belt_bounds, belt)) {
            return {};
        }
        projected = apply(robot->base_from_belt, belt);
        error += robot->residual_mm;
    }
    if (!finite(projected) || !std::isfinite(error)) {
        return {};
    }
    return ProjectedPoint{Quality::Valid, projected, error};
}

std::optional<RigidTransform> robot_pose(const Snapshot& snapshot,
                                         const RigidTransform& belt_from_tcp,
                                         std::string_view tool_id, PoseTarget target) {
    const auto& robot = snapshot.robot;
    if ((target != PoseTarget::Tcp && target != PoseTarget::Flange) ||
        !robot || robot->tool_id != tool_id || !valid_transform(belt_from_tcp) ||
        !contains(robot->belt_bounds, belt_from_tcp.translation)) {
        return {};
    }
    auto pose = compose(robot->base_from_belt, belt_from_tcp);
    if (target == PoseTarget::Flange) {
        pose = compose(pose, inverse(robot->flange_from_tcp));
    }
    if (!valid_transform(pose)) {
        return {};
    }
    return pose;
}

}  // namespace waste::m02
