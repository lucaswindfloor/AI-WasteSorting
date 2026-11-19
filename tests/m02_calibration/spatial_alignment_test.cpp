#include "fixtures.h"
#include "m02_calibration/encoder_reference.h"
#include "m02_calibration/spatial_alignment.h"

namespace waste::m02::test {

void test_space() {
    const auto c = acquisition_config();
    const auto s = calibration(c);
    auto columns = project_scan(s.rgb, 25);
    check(columns.size() == 2 && near(columns[0].position.y, 407) && near(columns[0].position.z, 25),
          "height-dependent column mapping failed");
    check(project_scan(s.rgb, 60).empty(), "height extrapolated without calibration");
    auto masked = s.rgb;
    masked.layers.back().valid_columns = {0, 1};
    check(project_scan(masked, 25)[0].quality == Quality::Invalid &&
          project_scan(masked, 0)[0].quality == Quality::Valid, "height coverage mask was ignored");

    auto depth = row(m01::Source::E05, 14, "depth", 15ms, std::vector<std::byte>(10));
    auto bytes = *depth.payload;
    bytes[8] = std::byte{1};
    depth.payload = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
    depth.header.bytes = {0, 8};
    depth.header.valid_mask = m01::ByteRange{8, 2};
    depth.header.id.row = 1;
    auto points = project_depth(s, c.e05->format, depth);
    check(points.size() == 2 && points[0].quality == Quality::Valid && near(points[0].position.z, 0) &&
          near(points[0].position.x, 1) && points[1].quality == Quality::Missing,
          "zero height and masked height were not distinguished");

    auto encoder = s.encoder;
    encoder.signed_mm_per_count = 0.1;
    const auto station = belt_shift(encoder, {7, 32015, 0, CountBasis::SourceExposureLatch},
                                    {7, 30515, 0, CountBasis::SourceExposureLatch});
    check(station && near(station->mm, -150), "cross-station reference count is incorrect");
    const std::vector<ProjectedPoint> measurement{{Quality::Valid, {150, 406, 25}, 0}};
    check(near(shift_points(measurement, *station)[0].position.x, 0), "station shift applied incorrectly");

    const auto p = transform_point(s, Frame::Belt, Frame::RobotBase, {5, 406, 10});
    check(p && near(p->position.x, 105) && near(p->position.z, 15), "robot transform is reversed");
    check(!transform_point(s, Frame::Belt, Frame::RobotBase, {500, 406, 10}), "out-of-domain point accepted");
    auto tcp = RigidTransform{};
    tcp.translation = {5, 406, 10};
    const auto target = robot_pose(s, tcp, "vacuum", PoseTarget::Tcp);
    const auto flange = robot_pose(s, tcp, "vacuum", PoseTarget::Flange);
    check(target && flange && near(target->translation.z, 15) && near(flange->translation.z, -25),
          "TCP offset was repeated or applied in the wrong direction");
    check(!robot_pose(s, tcp, "other-tool", PoseTarget::Flange), "wrong tool accepted");
    tcp.rotation[0] = -1;
    check(!robot_pose(s, tcp, "vacuum", PoseTarget::Tcp), "reflection accepted as a robot rotation");
}

}  // namespace waste::m02::test
