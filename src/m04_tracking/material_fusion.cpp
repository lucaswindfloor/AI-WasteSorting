#include "material_fusion.h"
#include "belt_prediction.h"

#include <map>
#include <utility>

namespace waste::m04 {

Material fuse_material(const TrackingConfig& c, std::span<const SpectralPoint* const> owned,
                       std::vector<SampleKey> ambiguous) {
    Material result;
    std::map<std::string, std::vector<SampleKey>> supporting;
    for (const auto* point : owned) {
        const auto& readings = point->readings;
        if (readings.empty()) {
            continue;
        }
        const auto& first = readings.front().label;
        if (std::ranges::all_of(readings, [&](const auto& reading) { return reading.label == first; })) {
            supporting[first].push_back(point->sample.source.key);
        } else {
            result.disputed.push_back(point->sample.source.key);
        }
    }
    const auto winner = std::ranges::max_element(supporting, {}, [](const auto& item) {
        return item.second.size();
    });
    const bool tied = winner != supporting.end() &&
        std::ranges::count_if(supporting, [&](const auto& item) {
            return item.second.size() == winner->second.size();
        }) > 1;
    if (winner != supporting.end() && !tied && winner->second.size() >= c.min_material_samples &&
        winner->second.size() >= c.min_material_fraction * owned.size()) {
        result.status = MaterialStatus::Known;
        result.label = winner->first;
        result.supporting = winner->second;
        for (const auto* point : owned) {
            const auto expiry = reference_expiry(point->sample.reference, c.max_evidence_age);
            result.valid_until = std::min(result.valid_until.value_or(expiry), expiry);
        }
    } else if (supporting.size() > 1 || !result.disputed.empty()) {
        result.status = MaterialStatus::Conflict;
    } else if (!ambiguous.empty()) {
        result.status = MaterialStatus::AmbiguousOwnership;
    }
    result.disputed.insert(result.disputed.end(), ambiguous.begin(), ambiguous.end());
    std::ranges::sort(result.disputed);
    return result;
}

}  // namespace waste::m04
