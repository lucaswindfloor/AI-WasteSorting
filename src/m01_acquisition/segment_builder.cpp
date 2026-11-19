#include "segment_builder.h"

#include <array>
#include <utility>

namespace waste::m01 {
namespace {

std::array<const Observation*, 2> observations(const SegmentItem& item) {
    if (const auto* trigger = std::get_if<TriggerRecord>(&item)) {
        return {trigger->rgb ? &*trigger->rgb : nullptr, trigger->hsi ? &*trigger->hsi : nullptr};
    }
    return {std::get_if<Observation>(&item), nullptr};
}

Diagnostic dropped_item(const SegmentItem& item) {
    if (const auto* trigger = std::get_if<TriggerRecord>(&item)) {
        return {Issue::ConsumerBacklog, Source::Sync, trigger->event.key, {}, {}, {}, trigger->event};
    }
    const auto& observation = std::get<Observation>(item).header;
    return {Issue::ConsumerBacklog, observation.id.source, observation.trigger, {},
            observation.id, {}, {}};
}

}  // namespace

SegmentBuilder::SegmentBuilder(std::shared_ptr<const Config> config,
                               std::shared_ptr<std::atomic_size_t> inflight)
    : config_(std::move(config)), inflight_(std::move(inflight)) {}

void SegmentBuilder::add(SegmentItem item, Tick now, Output& out) {
    if (auto* diagnostic = std::get_if<Diagnostic>(&item)) {
        if (diagnostic->issue == Issue::DuplicateConflict && diagnostic->first_trigger) {
            for (auto& buffered : items_) {
                auto* record = std::get_if<TriggerRecord>(&buffered);
                if (!record || record->event.key != *diagnostic->first_trigger) {
                    continue;
                }
                if (diagnostic->source == Source::Rgb || diagnostic->source == Source::Sync) {
                    record->rgb_quality = Quality::Unknown;
                }
                if (diagnostic->source == Source::Hsi || diagnostic->source == Source::Sync) {
                    record->hsi_quality = Quality::Unknown;
                }
            }
        }
        out.diagnostics.push_back(std::move(*diagnostic));
        return;
    }
    const auto rows = observations(item);
    auto additional = [&] {
        std::size_t total = 0;
        const void* previous = nullptr;
        for (const auto* row : rows) {
            if (row && previous != row->payload.get() && !payloads_.contains(row->payload.get())) {
                previous = row->payload.get();
                total += row->payload->size();
            }
        }
        return total;
    };
    auto extra = additional();
    if (!items_.empty() && extra > config_->max_segment_bytes - bytes_) {
        flush(out);
        extra = additional();
    }
    if (!opened_) {
        opened_ = now;
    }
    for (const auto* row : rows) {
        if (row) {
            payloads_.insert(row->payload.get());
        }
    }
    bytes_ += extra;
    items_.push_back(std::move(item));
    if (items_.size() >= config_->max_segment_items || bytes_ >= config_->max_segment_bytes) {
        flush(out);
    }
}

void SegmentBuilder::expire(Tick now, Output& out) {
    if (opened_ && now - *opened_ >= config_->segment_timeout) {
        flush(out);
    }
}

void SegmentBuilder::flush(Output& out) {
    if (items_.empty()) {
        return;
    }
    if (inflight_->load() >= config_->max_inflight_segments) {
        for (const auto& item : items_) {
            out.diagnostics.push_back(dropped_item(item));
        }
    } else {
        auto segment = std::make_unique<const Segment>(
            Segment{config_->run_id, config_->generation, sequence_++, config_, std::move(items_)});
        inflight_->fetch_add(1);
        out.segments.emplace_back(segment.release(), [inflight = inflight_](const Segment* value) {
            delete value;
            inflight->fetch_sub(1);
        });
    }
    items_.clear();
    payloads_.clear();
    bytes_ = 0;
    opened_.reset();
}

}  // namespace waste::m01
