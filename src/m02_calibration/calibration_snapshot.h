#pragma once

#include "calibration_types.h"

#include <map>

namespace waste::m02 {

class CalibrationSnapshots {
public:
    // Installing an identical run/generation twice is rejected.
    void install(Snapshot snapshot);
    std::shared_ptr<const Snapshot> find(const std::string& run_id, std::uint64_t generation) const;
    void retire(const std::string& run_id, std::uint64_t generation);

private:
    using Key = std::pair<std::string, std::uint64_t>;
    // Keep a generation's identity after retirement so it cannot be reused.
    std::map<Key, std::shared_ptr<const Snapshot>> snapshots_;
};

bool matches(const Snapshot& snapshot, const m01::Config& config);

}  // namespace waste::m02
