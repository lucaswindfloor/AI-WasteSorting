#include "contact_window.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace waste::m06 {
namespace {

bool valid_ref(const m04::Reference& r) {
    return r.time >= Tick::zero() && r.time_error >= Tick::zero() && std::isfinite(r.count) &&
           std::isfinite(r.count_error) && r.count_error >= 0;
}

std::optional<Tick> travel(long double distance, double speed, bool upper) {
    const auto ns = distance * 1'000'000'000.L / speed;
    const auto rounded = upper ? std::ceil(ns) : std::floor(ns);
    const auto exclusive_limit = std::ldexp(1.L, std::numeric_limits<Tick::rep>::digits);
    if (!std::isfinite(rounded) || rounded < 0 || rounded >= exclusive_limit) {
        return {};
    }
    return Tick(static_cast<Tick::rep>(rounded));
}

}  // namespace

ContactComputation contact_window(const SchedulingConfig& c, const m05::Candidate& candidate,
                                  const m04::Prediction& p, const MotionForecast& f,
                                  const ContactRegion& r, Tick now) {
    if (candidate.target.generation != c.generation || p.generation != c.generation ||
        f.generation != c.generation) {
        return {{}, Reason::WrongGeneration};
    }
    if (now < Tick::zero() || now >= c.valid_until || now >= candidate.valid_until ||
        candidate.contact.check.state != m05::State::Ready || !candidate.contact.center) {
        return {{}, Reason::CandidateStale};
    }
    if (p.reason != m04::Reason::None || p.target != candidate.target.id ||
        p.version != candidate.target.version || p.origin != candidate.target.reference ||
        !valid_ref(p.origin) || !valid_ref(p.current) ||
        p.origin.encoder_epoch != p.current.encoder_epoch || p.current.time < p.origin.time ||
        p.current.time > now || now - p.current.time >= c.max_prediction_age ||
        now >= p.valid_until || !std::isfinite(p.applied_mm) ||
        !std::isfinite(p.error_mm) || p.error_mm < 0 || p.error_mm > c.max_tracking_error_mm) {
        return {{}, Reason::PredictionInvalid};
    }
    if (f.reference != p.current || f.version.empty() || now >= f.valid_until ||
        !std::isfinite(f.min_mm_per_second) || !std::isfinite(f.max_mm_per_second) ||
        f.min_mm_per_second <= 0 || f.max_mm_per_second < f.min_mm_per_second) {
        return {{}, Reason::MotionInvalid};
    }

    auto center = *candidate.contact.center;
    center.x += p.applied_mm;
    const auto error = static_cast<long double>(p.error_mm) + c.forecast_extra_error_mm;
    if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z) ||
        !std::isfinite(static_cast<double>(error)) ||
        center.y - error < r.min.y || center.y + error > r.max.y ||
        center.z - error < r.min.z || center.z + error > r.max.z) {
        return {{}, Reason::NoContactWindow};
    }

    const auto latest_entry_distance = r.min.x - (center.x - error);
    const auto earliest_exit_distance = r.max.x - (center.x + error);
    if (earliest_exit_distance < 0) {
        return {{}, Reason::NoContactWindow};
    }
    const auto entry = travel(std::max(0.L, latest_entry_distance), f.min_mm_per_second, true);
    const auto exit = travel(earliest_exit_distance, f.max_mm_per_second, false);
    if (!entry || !exit) {
        return {{}, Reason::NoContactWindow};
    }
    const auto late_reference = add_ticks(p.current.time, p.current.time_error);
    const auto open = late_reference ? add_ticks(*late_reference, *entry) : std::nullopt;
    const auto early_reference = std::max(Tick::zero(), p.current.time - p.current.time_error);
    const auto close = add_ticks(early_reference, *exit);
    const auto expires = std::min({c.valid_until, candidate.valid_until, p.valid_until, f.valid_until});
    if (!open || !close) {
        return {{}, Reason::NoContactWindow};
    }
    const Window window{std::max(now, *open), std::min(*close, expires - Tick(1))};
    if (!valid(window)) {
        return {{}, Reason::NoContactWindow};
    }
    return {ContactWindow{window, center, p.current, static_cast<double>(error), r.id}, Reason::None};
}

}  // namespace waste::m06
