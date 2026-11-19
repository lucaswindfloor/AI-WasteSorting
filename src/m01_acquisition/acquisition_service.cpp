#include "acquisition_service.h"
#include "device_session.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace waste::m01 {

void AcquisitionService::check_clock(Tick now) {
    if (now < now_) {
        throw std::invalid_argument("acquisition clock must be monotonic");
    }
    now_ = now;
}

void AcquisitionService::start(Config config, Tick now) {
    if (matcher_ || (config_ && config.generation <= config_->generation)) {
        throw std::logic_error("close the current generation before starting a newer one");
    }
    validate_config(config);
    check_clock(now);
    config_ = std::make_shared<const Config>(std::move(config));
    matcher_.emplace(*config_);
    builder_.emplace(config_, inflight_);
    last_sync_.reset();
    last_rgb_.reset();
    last_hsi_.reset();
    last_e05_.reset();
    last_pair_.reset();
    rejected_ = unresolved_ = 0;
}

void AcquisitionService::collect(std::vector<SegmentItem> items, Output& out) {
    for (auto& item : items) {
        builder_->add(std::move(item), now_, out);
    }
}

void AcquisitionService::account(const Output& out, std::size_t first_segment,
                                 std::size_t first_diagnostic) {
    const auto mark = [](std::optional<Tick>& last, Tick received) {
        last = std::max(last.value_or(received), received);
    };
    for (std::size_t i = first_segment; i < out.segments.size(); ++i) {
        for (const auto& item : out.segments[i]->items) {
            if (const auto* record = std::get_if<TriggerRecord>(&item)) {
                mark(last_sync_, record->received);
                if (record->rgb && record->rgb_quality == Quality::Valid) {
                    mark(last_rgb_, record->rgb->received);
                }
                if (record->hsi && record->hsi_quality == Quality::Valid) {
                    mark(last_hsi_, record->hsi->received);
                }
                if (record->rgb && record->hsi && record->rgb_quality == Quality::Valid &&
                    record->hsi_quality == Quality::Valid) {
                    mark(last_pair_, std::min({record->received, record->rgb->received,
                                               record->hsi->received}));
                }
            } else if (const auto* row = std::get_if<Observation>(&item);
                       row && row->header.source_quality == Quality::Valid) {
                switch (row->header.id.source) {
                case Source::Rgb: mark(last_rgb_, row->received); break;
                case Source::Hsi: mark(last_hsi_, row->received); break;
                case Source::E05: mark(last_e05_, row->received); break;
                case Source::Sync: break;
                }
            }
        }
    }
    unresolved_ += out.diagnostics.size() - first_diagnostic;
}

Output AcquisitionService::advance(Tick now) {
    check_clock(now);
    Output result{config_, {}, {}};
    if (matcher_) {
        builder_->expire(now, result);
        collect(matcher_->expire(now), result);
        account(result, 0, 0);
    }
    return result;
}

Output AcquisitionService::ingest(const SyncInput& input, Tick now) {
    auto result = advance(now);
    const auto diagnostics_before = result.diagnostics.size();
    const auto segments_before = result.segments.size();
    const bool current = matcher_ && input.generation == config_->generation;
    if (!current || !valid_sync(input, *config_) ||
        (input.host_received && (*input.host_received < Tick::zero() || *input.host_received > now))) {
        auto issue = Issue::OldGeneration;
        if (current) {
            const auto& key = input.event.key;
            issue = key.sync_session != config_->sync.session || key.encoder_epoch != config_->encoder_epoch
                        ? Issue::WrongSession : Issue::InvalidPacket;
        }
        result.diagnostics.push_back(
            {issue, Source::Sync, input.event.key, {}, {}, {}, input.event, input.generation});
        ++rejected_;
    } else {
        collect(matcher_->sync(input.event, now, input.host_received.value_or(now)), result);
    }
    account(result, segments_before, diagnostics_before);
    return result;
}

void AcquisitionService::reject(Issue issue, const PacketView& packet, Output& out) {
    ++rejected_;
    // Valid-sized batches retain the affected source/row identities without owning SDK bytes.
    if (packet.observations.empty() || !config_ ||
        packet.observations.size() > config_->max_packet_items) {
        out.diagnostics.push_back({issue, packet.source, {}, {}, {}, {}, {}, packet.generation});
        return;
    }
    for (const auto& row : packet.observations) {
        out.diagnostics.push_back({issue, packet.source, row.trigger, {}, row.id, {}, {}, packet.generation});
    }
}

Output AcquisitionService::ingest(const PacketView& packet, Tick now) {
    auto result = advance(now);
    const auto diagnostics_before = result.diagnostics.size();
    const auto segments_before = result.segments.size();
    if (!matcher_ || packet.generation != config_->generation) {
        reject(Issue::OldGeneration, packet, result);
    } else if (const auto* source = source_config(*config_, packet.source);
               !source || packet.session != source->session) {
        reject(Issue::WrongSession, packet, result);
    } else if (packet.payload.size() > config_->max_packet_bytes ||
               packet.observations.size() > config_->max_packet_items ||
               packet.format != source->format ||
               (packet.host_received && (*packet.host_received < Tick::zero() || *packet.host_received > now)) ||
               !(valid_rgb(packet) || valid_hsi(packet) || valid_e05(packet))) {
        reject(Issue::InvalidPacket, packet, result);
    } else if (auto payload = buffer_.copy(packet.payload, config_->buffer_capacity)) {
        for (const auto& header : packet.observations) {
            collect(matcher_->observe(Observation{header, packet.host_received.value_or(now), payload}, now), result);
        }
    } else {
        reject(Issue::Capacity, packet, result);
    }
    account(result, segments_before, diagnostics_before);
    return result;
}

Output AcquisitionService::close(Tick now, std::optional<TriggerKey> last_expected) {
    if (last_expected && !matcher_) {
        throw std::logic_error("stop boundary needs an active generation");
    }
    const auto trailing = last_expected ? matcher_->trailing_gap(*last_expected) : std::nullopt;
    auto result = advance(now);
    if (matcher_) {
        const auto diagnostics_before = result.diagnostics.size();
        const auto segments_before = result.segments.size();
        if (trailing) {
            result.diagnostics.push_back(*trailing);
        }
        collect(matcher_->expire(now, true), result);
        builder_->flush(result);
        account(result, segments_before, diagnostics_before);
        matcher_.reset();
        builder_.reset();
    }
    return result;
}

Health AcquisitionService::health() const {
    const auto bytes = buffer_.used();
    const auto segments = inflight_->load();
    const auto pending = matcher_ ? matcher_->pending() : 0;
    const auto pressure = config_ && (bytes >= config_->buffer_high_water ||
                                      segments >= config_->max_inflight_segments ||
                                      pending >= config_->max_pending);
    const auto fresh = [&](const std::optional<Tick>& last) {
        return last && now_ - *last < config_->source_stale_timeout;
    };
    const auto observed = config_ && (config_->routing == Routing::SharedTrigger
                                         ? fresh(last_pair_)
                                         : fresh(last_rgb_) && fresh(last_hsi_));
    return {matcher_.has_value(), matcher_ && !pressure && observed && (!config_->e05 || fresh(last_e05_)),
            pressure, bytes, segments, pending,
            matcher_ ? matcher_->oldest_age(now_) : Tick{}, now_,
            last_sync_, last_rgb_, last_hsi_, last_e05_, last_pair_, rejected_, unresolved_};
}

}  // namespace waste::m01
