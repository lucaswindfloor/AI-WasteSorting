#pragma once

#include "tracking_config.h"

namespace waste::m04 {

struct MaterialReading {
    Origin origin;
    std::string label;
    float score{};
};

struct SpectralPoint {
    Sample sample;
    std::vector<MaterialReading> readings;
};

class EvidenceIndex {
public:
    explicit EvidenceIndex(Limits limits);
    Reason add_material(Sample sample, MaterialReading reading);
    Reason add_surface(Sample sample);
    void expire(Tick now, Tick max_age);
    void discard_before(Tick cutoff);
    const std::map<SampleKey, SpectralPoint>& material() const;
    const std::map<SampleKey, Sample>& surfaces() const;

private:
    std::size_t size() const;
    Limits limits_;
    std::map<SampleKey, SpectralPoint> material_;
    std::map<SampleKey, Sample> surfaces_;
};

}  // namespace waste::m04
