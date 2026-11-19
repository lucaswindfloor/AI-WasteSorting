#pragma once

#include "belt_prediction.h"
#include "evidence_index.h"
#include "instance_association.h"
#include "target_store.h"

namespace waste::m04 {

class TrackingService {
public:
    // Call all entry points on one serialized lane in E09 monotonic time.
    Report activate(TrackingConfig config, Tick now);
    Report ingest_window(const m03::Result& window, Tick now);
    Report ingest_surfaces(const m02::Result& observations, Tick now);
    Report update_encoder(EncoderState encoder, Tick now);
    Report advance(Tick now);
    Report invalidate_motion(TargetId id, std::uint64_t expected_version, Tick now);
    std::optional<Target> target(TargetId id) const;
    Prediction predict(TargetId id, std::uint64_t expected_version, Tick now) const;

private:
    void step(Tick now, Report& report);
    void check_time(Tick now);
    void fail_motion(Reason reason);
    void check_targets();
    void retire(TargetId id, Reason reason);
    std::vector<const Target*> retired_for(Tick observation_time) const;
    void refresh_evidence();
    void apply_instances(std::span<const Instance> instances, Report& report);
    void close(Report& report);

    std::optional<TrackingConfig> config_;
    std::optional<EvidenceIndex> evidence_;
    std::map<std::string, std::uint64_t> highest_generation_;
    std::map<WindowKey, Tick> seen_windows_;
    TargetStore targets_;
    std::optional<EncoderState> encoder_;
    struct Retired {
        Target contour;
        Tick retired_at;
    };
    std::map<TargetId, Retired> retired_;
    std::optional<Tick> last_encoder_time_, motion_barrier_, birth_barrier_;
    std::optional<Reason> motion_fault_;
    Tick now_{};
};

}  // namespace waste::m04
