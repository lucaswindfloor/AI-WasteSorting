#pragma once

#include "inference_types.h"

namespace waste::m03 {

struct Channels {
    std::string encoding;
    std::vector<std::string> bands;
    m02::SpectralLevel spectral_level{m02::SpectralLevel::Raw};
    std::string spectral_reference;
    // For valid samples: (sample - offset[channel]) * scale[channel].
    std::vector<float> offset, scale;
};

struct WindowSpec {
    std::size_t rows{}, step{}, max_missing_rows{}, max_buffered_rows{};
    Tick max_wait{};
    bool accepts_mask{};
    float min_valid_fraction{1};
};

struct Raster {
    // Crop in the reference grid, resize with nearest pixel center, then pad.
    std::size_t top{}, left{}, rows{}, columns{}, resized_rows{}, resized_columns{};
    std::size_t pad_top{}, pad_left{}, pad_bottom{}, pad_right{};
    float fill{};
    std::size_t height() const;
    std::size_t width() const;
};

struct Label {
    std::string name;
    EvidenceKind kind{};
};

struct Manifest {
    ModelIdentity identity;
    Route route{};
    std::optional<Channels> rgb, hsi;
    WindowSpec window;
    Raster raster;
    std::vector<Label> labels;
    ScoreKind score_kind{ScoreKind::Probability};
    float threshold{};
    std::size_t max_candidates{};
};

void validate(const Manifest& manifest);
void verify_compatibility(const Manifest& manifest, const m01::Config& acquisition,
                          const m02::Snapshot& calibration);

}  // namespace waste::m03
