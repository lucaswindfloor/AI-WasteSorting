#include "fixtures.h"

namespace waste::m04::test {

void prediction_test() {
    auto c = settings();
    auto hit = sample(m01::Source::Rgb, 10, 406, 10ms, 33815, 30);
    hit.source_reference = ref(10ms, 33815);
    hit.upstream_shift_mm = 30;
    const auto footprint = *make_footprint(c, {hit});
    const Target target{42, 3, c.generation, TargetState::Tracked, Reason::None,
                        footprint, {}, {}, 300ms, 10ms};
    auto motion = encoder(c, 110ms, 37815);
    const auto prediction = predict_position(c, target, &motion, 110ms);
    check(prediction.reason == Reason::None && near(prediction.applied_mm, 200) &&
          near(prediction.position.x, 230) && prediction.version == 3 &&
          prediction.origin.count == 33815 && prediction.current.count == 37815 && prediction.error_mm > 2,
          "T042 prediction reapplied the M02 shift or lost version/uncertainty");
    check(prediction.valid_until == target.valid_until && prediction.fresh_until == 260ms - 1ns,
          "target validity and current encoder freshness were conflated");
    check(predict_position(c, target, &motion, 260ms).reason == Reason::EncoderStale,
          "stale count used for online prediction");
    motion.reference.encoder_epoch = 8;
    check(predict_position(c, target, &motion, 110ms).reason == Reason::EncoderReset,
          "new count epoch was used for old target");
    motion = encoder(c, 110ms, 44815);
    check(predict_position(c, target, &motion, 110ms).reason == Reason::OutsideRange,
          "forecast exceeded validated displacement");
    motion.following_verified = false;
    check(predict_position(c, target, &motion, 110ms).reason == Reason::MotionUnverified,
          "unverified motion forecast passed");
    motion = encoder(c, 110ms, 33500);
    check(predict_position(c, target, &motion, 110ms).reason == Reason::MotionUnverified,
          "unexpected reversal passed");
}

}  // namespace waste::m04::test
