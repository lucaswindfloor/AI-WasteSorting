#include "encoder_reference.h"

#include <algorithm>
#include <cmath>

namespace waste::m02 {

CountEstimate locked_count(const EncoderConfig& config, const m01::TriggerEvent& trigger) {
    return {trigger.key.encoder_epoch, static_cast<long double>(trigger.encoder_count),
            config.lock_error_counts, CountBasis::TriggerLatch};
}

std::optional<CountEstimate> observation_count(const EncoderConfig& config,
                                               const m01::ObservationHeader& header,
                                               const std::optional<TimeEstimate>& time,
                                               std::span<const CountSample> samples) {
    if (header.encoder_epoch && *header.encoder_epoch != config.epoch) {
        return {};
    }
    if (header.encoder_count && header.encoder_epoch == config.epoch &&
        std::ranges::find(config.verified_native_sources, header.id.source) !=
            config.verified_native_sources.end()) {
        return CountEstimate{config.epoch, static_cast<long double>(*header.encoder_count),
                             config.native_error_counts, CountBasis::SourceExposureLatch};
    }
    if (!time || time->error < m01::Tick::zero()) {
        return {};
    }
    // Samples are ordered by mapped time, with no duplicate times in this epoch.
    const auto upper = std::ranges::upper_bound(samples, time->value, {},
                                                [](const CountSample& sample) { return sample.time.value; });
    if (upper == samples.begin() || upper == samples.end()) {
        return {};
    }
    const auto& before = *(upper - 1);
    const auto& after = *upper;
    if (before.epoch != config.epoch || after.epoch != config.epoch ||
        before.time.error < m01::Tick::zero() || after.time.error < m01::Tick::zero() ||
        before.direction != after.direction || (before.direction == 0 && before.count != after.count)) {
        return {};
    }
    const auto start = static_cast<long double>(before.time.value.count());
    const auto end = static_cast<long double>(after.time.value.count());
    const auto at = static_cast<long double>(time->value.count());
    const auto span = end - start;
    if (span <= 0 || span + before.time.error.count() + after.time.error.count() >
                         config.max_sample_gap.count() ||
        at - time->error.count() < start + before.time.error.count() ||
        at + time->error.count() > end - after.time.error.count()) {
        return {};
    }
    const auto delta = static_cast<long double>(after.count) - before.count;
    const auto minimum_span = span - before.time.error.count() - after.time.error.count();
    if (std::abs(delta) * 1e9L > config.max_count_rate_per_second * minimum_span) {
        return {};
    }
    const auto time_error = static_cast<long double>(time->error.count()) +
                            std::max(before.time.error, after.time.error).count();
    const auto error = config.lock_error_counts + config.interpolation_error_counts +
                       static_cast<double>(time_error * config.max_count_rate_per_second / 1e9L);
    if (!std::isfinite(error)) {
        return {};
    }
    return CountEstimate{config.epoch, before.count + delta * ((at - start) / span),
                         error, CountBasis::Interpolation};
}

std::optional<BeltShift> belt_shift(const EncoderConfig& config,
                                    const CountEstimate& from, const CountEstimate& to) {
    if (from.epoch != config.epoch || to.epoch != config.epoch ||
        from.error_counts < 0 || to.error_counts < 0) {
        return {};
    }
    const auto mm = static_cast<double>((to.value - from.value) * config.signed_mm_per_count);
    const auto error = (from.error_counts + to.error_counts) * std::abs(config.signed_mm_per_count);
    if (!std::isfinite(mm) || !std::isfinite(error) || std::abs(mm) + error > config.max_displacement_mm) {
        return {};
    }
    return BeltShift{mm, error, from, to};
}

}  // namespace waste::m02
