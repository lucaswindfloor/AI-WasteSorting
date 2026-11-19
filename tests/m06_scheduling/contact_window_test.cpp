#include "fixtures.h"

namespace waste::m06::test {

void contact_test() {
    const auto c = config();
    const m05::EligibilityService m05(recipe());
    const auto data = snapshot();
    auto t = current();
    const auto entry = queued(m05, data, t);
    const auto& region = c.lanes[0].contact_regions[0];
    const auto fit = contact_window(c, entry.candidate, t.prediction, t.forecast, region, 50ms);
    check(fit.window && fit.window->instants == Window{145ms, 175ms} &&
          near(fit.window->current_pick.x, 80) && near(fit.window->current_pick.y, 406),
          "T042 contact window used target center or reapplied motion");
    auto point = entry.candidate;
    point.contact.center->x = 20;
    const auto offset = contact_window(c, point, t.prediction, t.forecast, region, 50ms);
    check(offset.window && offset.window->instants == Window{135ms, 165ms},
          "offset pick location did not change the time window");
    t.forecast.min_mm_per_second = 1500;
    t.forecast.max_mm_per_second = 2600;
    check(!contact_window(c, entry.candidate, t.prediction, t.forecast, region, 50ms).window,
          "empty guaranteed window accepted under variable speed");
    t = current();
    t.prediction.current.encoder_epoch++;
    check(contact_window(c, entry.candidate, t.prediction, t.forecast, region, 50ms).reason ==
              Reason::PredictionInvalid, "changed encoder epoch accepted");
    t = current();
    t.forecast.min_mm_per_second = 0;
    check(contact_window(c, entry.candidate, t.prediction, t.forecast, region, 50ms).reason ==
              Reason::MotionInvalid, "stopped belt forecast accepted");
    t = current();
    t.forecast.valid_until = 160ms;
    const auto limited = contact_window(c, entry.candidate, t.prediction, t.forecast, region, 50ms);
    check(limited.window && limited.window->instants.close == 160ms - 1ns,
          "forecast validity not intersected with the contact window");
}

}  // namespace waste::m06::test
