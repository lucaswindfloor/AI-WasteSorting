#include "output_decoder.h"

#include <algorithm>
#include <cmath>

namespace waste::m03 {

std::string validate_output(const Manifest& manifest, const BackendCompletion& output) {
    if (!output.error.empty()) {
        return output.error;
    }
    if (output.candidates.size() > manifest.max_candidates) {
        return "too_many_candidates";
    }
    for (const auto& candidate : output.candidates) {
        const auto& box = candidate.box;
        if (candidate.class_index >= manifest.labels.size() || !std::isfinite(candidate.score) ||
            (manifest.score_kind == ScoreKind::Probability && (candidate.score < 0 || candidate.score > 1)) ||
            box.x0 >= box.x1 || box.y0 >= box.y1 ||
            box.x1 > manifest.raster.width() || box.y1 > manifest.raster.height()) {
            return "invalid_class_score_or_box";
        }
        if (!candidate.mask.empty() &&
            (candidate.mask.size() != (box.x1 - box.x0) * (box.y1 - box.y0) ||
             std::ranges::any_of(candidate.mask, [](auto byte) { return byte > 1; }))) {
            return "invalid_candidate_mask";
        }
    }
    return {};
}

}  // namespace waste::m03
