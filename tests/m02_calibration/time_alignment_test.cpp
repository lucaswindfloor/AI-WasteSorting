#include "fixtures.h"
#include "m02_calibration/encoder_reference.h"
#include "m02_calibration/time_alignment.h"

namespace waste::m02::test {

void test_time() {
    auto s = calibration(acquisition_config());
    const auto t = trigger(2147, 10ms, 1000);
    const m01::TriggerRecord record{t, 40ms, {}, {}, Quality::Missing, Quality::Missing};
    auto rgb = row(m01::Source::Rgb, 12, "rgb", 11ms, {std::byte{0}}, t.key);
    auto time = observation_time(s, rgb.header, &record);
    check(time && time->value == 11ms && time->error == 1ns, "exposure mapped from receive time");
    rgb.header.source_time->clock_domain = "wrong";
    check(!observation_time(s, rgb.header, &record), "unknown clock accepted");
    s.trigger_delays.push_back({m01::Source::Rgb, 12, 1ms, 10us});
    time = observation_time(s, rgb.header, &record);
    check(time && time->value == 11ms && time->error == 10us + 1ns, "verified delay fallback lost");
    rgb.header.association_verified = false;
    check(!observation_time(s, rgb.header, &record), "unverified trigger supplied exposure");

    std::array samples{CountSample{7, 1000, {10ms, 0ns, TimeBasis::SourceClock}, 1},
                       CountSample{7, 1020, {20ms, 0ns, TimeBasis::SourceClock}, 1}};
    auto count = observation_count(s.encoder, rgb.header, time, samples);
    check(count && near(static_cast<double>(count->value), 1002), "bad count interpolation");
    auto displacement = belt_shift(s.encoder, *count, locked_count(s.encoder, t));
    check(displacement && near(displacement->mm, -0.1), "bad reference motion direction");
    auto backwards = s.encoder;
    backwards.signed_mm_per_count = -0.05;
    displacement = belt_shift(backwards, *count, locked_count(backwards, t));
    check(displacement && near(displacement->mm, 0.1), "negative encoder direction lost");
    samples[1].direction = -1;
    check(!observation_count(s.encoder, rgb.header, time, samples), "reversal interpolated silently");
    samples[1].direction = 1;
    samples[1].epoch = 8;
    check(!observation_count(s.encoder, rgb.header, time, samples), "interpolated across reset");
    check(!within_time({10ms, 100us, TimeBasis::SourceClock}, {11ms, 100us, TimeBasis::SourceClock}, 1ms),
          "time tolerance ignored uncertainties");
    check(within_time({10ms, 100us, TimeBasis::SourceClock}, {11ms, 100us, TimeBasis::SourceClock}, 1200us),
          "nominal and uncertainty bounds not summed");
}

}  // namespace waste::m02::test
