#pragma once

#include "device_source.h"
#include "payload_buffer.h"
#include "segment_builder.h"
#include "trigger_matcher.h"

namespace waste::m01 {

// All calls use one serialized lane with nondecreasing, nonnegative host ticks.
class AcquisitionService {
public:
    AcquisitionService() = default;
    AcquisitionService(const AcquisitionService&) = delete;
    AcquisitionService& operator=(const AcquisitionService&) = delete;

    void start(Config config, Tick now);
    Output ingest(const SyncInput& input, Tick now);
    Output ingest(const PacketView& input, Tick now);
    Output advance(Tick now);
    Output close(Tick now, std::optional<TriggerKey> last_expected = {});
    Health health() const;

private:
    void check_clock(Tick now);
    void collect(std::vector<SegmentItem> items, Output& out);
    void account(const Output& out, std::size_t first_segment, std::size_t first_diagnostic);
    void reject(Issue issue, const PacketView& packet, Output& out);

    PayloadBuffer buffer_;
    std::shared_ptr<std::atomic_size_t> inflight_ = std::make_shared<std::atomic_size_t>(0);
    std::shared_ptr<const Config> config_;
    std::optional<TriggerMatcher> matcher_;
    std::optional<SegmentBuilder> builder_;
    Tick now_{};
    std::optional<Tick> last_sync_, last_rgb_, last_hsi_, last_e05_, last_pair_;
    std::uint64_t rejected_{}, unresolved_{};
};

}  // namespace waste::m01
