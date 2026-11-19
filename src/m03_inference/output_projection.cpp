#include "output_projection.h"

#include <set>
#include <utility>

namespace waste::m03 {

std::vector<Evidence> project_output(const Manifest& manifest, const PreparedInput& input,
                                     const BackendCompletion& output) {
    std::vector<Evidence> result;
    for (std::size_t i = 0; i < output.candidates.size(); ++i) {
        const auto& candidate = output.candidates[i];
        if (candidate.score < manifest.threshold) {
            continue;
        }
        const auto& label = manifest.labels[candidate.class_index];
        Evidence evidence{i, label.name, label.kind, manifest.score_kind, candidate.score, {}};
        std::set<SourceLocation> seen;
        const auto& box = candidate.box;
        for (auto y = box.y0; y < box.y1; ++y) {
            for (auto x = box.x0; x < box.x1; ++x) {
                if (!candidate.mask.empty() &&
                    !candidate.mask[(y - box.y0) * (box.x1 - box.x0) + x - box.x0]) {
                    continue;
                }
                const auto& pixel = input.source_map[y * input.width + x];
                for (const auto* sample : {&pixel.rgb, &pixel.hsi}) {
                    if (*sample && seen.insert((*sample)->source).second) {
                        evidence.samples.push_back(**sample);
                    }
                }
            }
        }
        if (!evidence.samples.empty()) {
            result.push_back(std::move(evidence));
        }
    }
    return result;
}

}  // namespace waste::m03
