#include "evidence_index.h"
#include "belt_prediction.h"

#include <utility>

namespace waste::m04 {

EvidenceIndex::EvidenceIndex(Limits limits) : limits_(limits) {}

std::size_t EvidenceIndex::size() const { return material_.size() + surfaces_.size(); }

Reason EvidenceIndex::add_material(Sample sample, MaterialReading reading) {
    const auto key = sample.source.key;
    const auto found = material_.find(key);
    if (found == material_.end()) {
        if (size() >= limits_.max_pending_samples) {
            return Reason::Capacity;
        }
        material_.emplace(key, SpectralPoint{std::move(sample), {std::move(reading)}});
    } else {
        auto& readings = found->second.readings;
        if (std::ranges::find(readings, reading.origin, &MaterialReading::origin) != readings.end()) {
            return Reason::Duplicate;
        }
        if (readings.size() >= limits_.max_readings_per_sample) {
            return Reason::Capacity;
        }
        readings.push_back(std::move(reading));
    }
    return Reason::None;
}

Reason EvidenceIndex::add_surface(Sample sample) {
    const auto key = sample.source.key;
    if (surfaces_.contains(key)) {
        return Reason::Duplicate;
    }
    if (size() >= limits_.max_pending_samples) {
        return Reason::Capacity;
    }
    surfaces_.emplace(key, std::move(sample));
    return Reason::None;
}

void EvidenceIndex::expire(Tick now, Tick max_age) {
    std::erase_if(material_, [&](const auto& entry) {
        return now >= reference_expiry(entry.second.sample.reference, max_age);
    });
    std::erase_if(surfaces_, [&](const auto& entry) {
        return now >= reference_expiry(entry.second.reference, max_age);
    });
}

void EvidenceIndex::discard_before(Tick cutoff) {
    std::erase_if(material_, [&](const auto& entry) { return entry.second.sample.reference.time <= cutoff; });
    std::erase_if(surfaces_, [&](const auto& entry) { return entry.second.reference.time <= cutoff; });
}

const std::map<SampleKey, SpectralPoint>& EvidenceIndex::material() const { return material_; }
const std::map<SampleKey, Sample>& EvidenceIndex::surfaces() const { return surfaces_; }

}  // namespace waste::m04
