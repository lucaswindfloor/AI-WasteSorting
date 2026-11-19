#pragma once

#include "acquisition_types.h"

#include <deque>
#include <map>

namespace waste::m01 {

class TriggerMatcher {
public:
    explicit TriggerMatcher(const Config& config);
    std::vector<SegmentItem> sync(const TriggerEvent& event, Tick now, Tick received);
    std::vector<SegmentItem> observe(Observation observation, Tick now);
    std::vector<SegmentItem> expire(Tick now, bool all = false);
    std::optional<Diagnostic> trailing_gap(TriggerKey last_expected) const;
    std::size_t pending() const;
    Tick oldest_age(Tick now) const;

private:
    struct Pending {
        Tick opened{};
        std::optional<Tick> sync_ingress;
        Tick sync_received{};
        std::optional<TriggerEvent> event;
        std::optional<Observation> rgb, hsi;
        bool rgb_hint{}, hsi_hint{};
        bool rgb_conflict{}, hsi_conflict{};
    };
    struct Seen {
        ObservationHeader header;
        std::uint64_t fingerprint{};
    };

    void finish(std::map<TriggerKey, Pending>::iterator entry, std::vector<SegmentItem>& out);
    bool remember(const Observation& observation, std::vector<SegmentItem>& out);
    void mark_conflict(const ObservationHeader& header);
    void finish_if_complete(std::map<TriggerKey, Pending>::iterator entry,
                            std::vector<SegmentItem>& out);

    const Config& config_;
    std::map<TriggerKey, Pending> pending_;
    std::optional<TriggerKey> latest_sync_;
    std::map<TriggerKey, TriggerEvent> recent_sync_;
    std::map<ObservationId, Seen> recent_rows_;
    std::deque<ObservationId> row_order_;
};

}  // namespace waste::m01
