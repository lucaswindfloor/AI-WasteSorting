#include "trigger_matcher.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace waste::m01 {
namespace {

Diagnostic observation_issue(Issue issue, const Observation& observation) {
    return {issue, observation.header.id.source, observation.header.trigger, {},
            observation.header.id, observation, {}};
}

Diagnostic sync_issue(Issue issue, const TriggerEvent& event) {
    return {issue, Source::Sync, event.key, {}, {}, {}, event};
}

std::uint64_t fingerprint(const Observation& observation) {
    // Bounded recent history stores a checksum, leaving payload ownership to consumers.
    auto value = std::uint64_t{14695981039346656037ULL};
    const auto add = [&](ByteRange range) {
        for (auto byte : std::span(*observation.payload).subspan(range.offset, range.size)) {
            value ^= std::to_integer<std::uint8_t>(byte);
            value *= 1099511628211ULL;
        }
    };
    add(observation.header.bytes);
    if (observation.header.valid_mask) {
        add(*observation.header.valid_mask);
    }
    return value;
}

Quality quality(const std::optional<Observation>& row, bool uncertain) {
    if (uncertain) {
        return Quality::Unknown;
    }
    return row ? row->header.source_quality : Quality::Missing;
}

}  // namespace

TriggerMatcher::TriggerMatcher(const Config& config) : config_(config) {}

std::vector<SegmentItem> TriggerMatcher::sync(const TriggerEvent& event, Tick now, Tick received) {
    std::vector<SegmentItem> out;
    if (const auto seen = recent_sync_.find(event.key); seen != recent_sync_.end()) {
        if (seen->second != event) {
            if (auto entry = pending_.find(event.key); entry != pending_.end()) {
                entry->second.rgb_conflict = true;
                entry->second.hsi_conflict = true;
            }
            out.emplace_back(sync_issue(Issue::DuplicateConflict, event));
        }
        return out;
    }
    if ((latest_sync_ && event.key <= *latest_sync_) ||
        (config_.first_expected_trigger && event.key.id < *config_.first_expected_trigger)) {
        out.emplace_back(sync_issue(Issue::Late, event));
        return out;
    }
    if (config_.contiguous_trigger_ids && event.key.id > 0) {
        auto previous = event.key;
        --previous.id;
        if (auto gap = trailing_gap(previous)) {
            out.emplace_back(std::move(*gap));
        }
    }
    latest_sync_ = event.key;
    recent_sync_.emplace(event.key, event);
    if (recent_sync_.size() > config_.recent_history) {
        recent_sync_.erase(recent_sync_.begin());
    }
    if (config_.routing == Routing::Independent) {
        out.emplace_back(TriggerRecord{event, received, {}, {}, Quality::Unknown, Quality::Unknown});
        return out;
    }
    // IF-001 is ordered. A camera-first entry behind this event cannot gain a sync record.
    for (auto entry = pending_.begin(); entry != pending_.lower_bound(event.key);) {
        if (!entry->second.event) {
            finish(entry++, out);
        } else {
            ++entry;
        }
    }
    auto entry = pending_.find(event.key);
    if (entry == pending_.end()) {
        if (pending_.size() >= config_.max_pending) {
            out.emplace_back(sync_issue(Issue::Capacity, event));
            return out;
        }
        entry = pending_.try_emplace(event.key).first;
        entry->second.opened = now;
    }
    entry->second.sync_ingress = now;
    entry->second.sync_received = received;
    entry->second.event = event;
    finish_if_complete(entry, out);
    return out;
}

void TriggerMatcher::mark_conflict(const ObservationHeader& header) {
    if (!header.trigger) {
        return;
    }
    if (auto entry = pending_.find(*header.trigger); entry != pending_.end()) {
        auto& value = entry->second;
        (header.id.source == Source::Rgb ? value.rgb_conflict : value.hsi_conflict) = true;
    }
}

bool TriggerMatcher::remember(const Observation& observation, std::vector<SegmentItem>& out) {
    const auto& header = observation.header;
    const auto checksum = fingerprint(observation);
    if (const auto found = recent_rows_.find(header.id); found != recent_rows_.end()) {
        if (found->second.header != header || found->second.fingerprint != checksum) {
            if (config_.routing == Routing::SharedTrigger && header.id.source != Source::E05) {
                mark_conflict(found->second.header);
                mark_conflict(header);
            }
            if (found->second.header.trigger && found->second.header.trigger != header.trigger) {
                out.emplace_back(Diagnostic{Issue::DuplicateConflict, header.id.source,
                                            found->second.header.trigger, {}, header.id, {}, {}});
            }
            out.emplace_back(observation_issue(Issue::DuplicateConflict, observation));
        }
        return false;
    }
    recent_rows_.emplace(header.id, Seen{header, checksum});
    row_order_.push_back(header.id);
    if (row_order_.size() > config_.recent_history) {
        recent_rows_.erase(row_order_.front());
        row_order_.pop_front();
    }
    return true;
}

std::vector<SegmentItem> TriggerMatcher::observe(Observation observation, Tick now) {
    std::vector<SegmentItem> out;
    const auto& header = observation.header;
    const auto* trigger = header.trigger ? &*header.trigger : nullptr;
    const auto source = header.id.source;
    // Keep an unproven observation's evidence without consuming its source identity:
    // the adapter may subsequently supply proof for the same row.
    if (source != Source::E05 && config_.routing == Routing::SharedTrigger &&
        (!trigger || !header.association_verified)) {
        if (trigger && trigger->sync_session == config_.sync.session &&
            trigger->encoder_epoch == config_.encoder_epoch &&
            (!latest_sync_ || *trigger > *latest_sync_ || pending_.contains(*trigger))) {
            if (!pending_.contains(*trigger) && pending_.size() < config_.max_pending) {
                pending_.try_emplace(*trigger).first->second.opened = now;
            }
            if (auto entry = pending_.find(*trigger); entry != pending_.end()) {
                auto& value = entry->second;
                (source == Source::Rgb ? value.rgb_hint : value.hsi_hint) = true;
            }
        }
        out.emplace_back(observation_issue(Issue::UnknownAssociation, observation));
        return out;
    }
    if (source != Source::E05 && config_.routing == Routing::SharedTrigger && trigger &&
        (trigger->sync_session != config_.sync.session ||
         trigger->encoder_epoch != config_.encoder_epoch)) {
        out.emplace_back(observation_issue(Issue::WrongSession, observation));
        return out;
    }
    if (!remember(observation, out)) {
        return out;
    }
    if (source == Source::E05 || config_.routing == Routing::Independent) {
        out.emplace_back(std::move(observation));
        return out;
    }
    auto entry = pending_.find(*trigger);
    if (entry == pending_.end()) {
        if (latest_sync_ && *trigger <= *latest_sync_) {
            out.emplace_back(observation_issue(Issue::Late, observation));
            return out;
        }
        if (pending_.size() >= config_.max_pending) {
            out.emplace_back(observation_issue(Issue::Capacity, observation));
            return out;
        }
        entry = pending_.try_emplace(*trigger).first;
        entry->second.opened = now;
    }
    auto& candidate = entry->second;
    auto& slot = source == Source::Rgb ? candidate.rgb : candidate.hsi;
    if (slot) {
        const auto same_row = slot->header == header && fingerprint(*slot) == fingerprint(observation);
        if (!same_row) {
            mark_conflict(header);
            out.emplace_back(observation_issue(Issue::DuplicateConflict, observation));
        }
        return out;
    }
    slot = std::move(observation);
    finish_if_complete(entry, out);
    return out;
}

void TriggerMatcher::finish_if_complete(std::map<TriggerKey, Pending>::iterator entry,
                                        std::vector<SegmentItem>& out) {
    const auto& item = entry->second;
    if (item.event && item.rgb && item.hsi) {
        finish(entry, out);
    }
}

void TriggerMatcher::finish(std::map<TriggerKey, Pending>::iterator entry,
                            std::vector<SegmentItem>& out) {
    auto& item = entry->second;
    if (item.event) {
        const auto rgb = quality(item.rgb, item.rgb_conflict || (!item.rgb && item.rgb_hint));
        const auto hsi = quality(item.hsi, item.hsi_conflict || (!item.hsi && item.hsi_hint));
        out.emplace_back(TriggerRecord{std::move(*item.event), item.sync_received,
                                       std::move(item.rgb), std::move(item.hsi), rgb, hsi});
    } else {
        for (const auto* row : {&item.rgb, &item.hsi}) {
            if (*row) {
                out.emplace_back(observation_issue(Issue::MissingSync, **row));
            }
        }
    }
    pending_.erase(entry);
}

std::vector<SegmentItem> TriggerMatcher::expire(Tick now, bool all) {
    std::vector<SegmentItem> out;
    for (auto entry = pending_.begin(); entry != pending_.end();) {
        // Camera-first retention starts at first evidence. Once IF-001 arrives,
        // its receipt starts the interval for the other camera.
        auto start = entry->second.sync_ingress.value_or(entry->second.opened);
        if (all || now - start >= config_.match_timeout) {
            finish(entry++, out);
        } else {
            ++entry;
        }
    }
    return out;
}

std::optional<Diagnostic> TriggerMatcher::trailing_gap(TriggerKey last_expected) const {
    if (!config_.contiguous_trigger_ids || last_expected.sync_session != config_.sync.session ||
        last_expected.encoder_epoch != config_.encoder_epoch) {
        throw std::invalid_argument("stop boundary needs the current contiguous sync source");
    }
    if (latest_sync_ && last_expected.id < latest_sync_->id) {
        throw std::invalid_argument("verified boundary precedes accepted sync data");
    }
    if (latest_sync_ && last_expected.id == latest_sync_->id) {
        return {};
    }
    const auto first_id = latest_sync_ ? std::optional(latest_sync_->id + 1)
                                       : config_.first_expected_trigger;
    if (!first_id || *first_id > last_expected.id) {
        return {};
    }
    auto first = last_expected;
    first.id = *first_id;
    return Diagnostic{Issue::MissingSync, Source::Sync, first, last_expected, {}, {}, {}};
}

std::size_t TriggerMatcher::pending() const {
    return pending_.size();
}

Tick TriggerMatcher::oldest_age(Tick now) const {
    Tick age{};
    for (const auto& [key, item] : pending_) {
        age = std::max(age, now - item.opened);
    }
    return age;
}

}  // namespace waste::m01
