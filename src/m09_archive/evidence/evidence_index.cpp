#include "evidence_index.h"

namespace waste::m09 {

EvidenceIndex::EvidenceIndex(const ArchiveService& archive, const Manifest& manifest) {
    for (const auto& key : manifest.records) {
        auto row = archive.record(manifest, key);
        if (!row.value) {
            unavailable.push_back(key);
            continue;
        }
        const auto& original = *row.value;
        if (!original.target.empty()) targets[original.target].push_back(key);
        for (const auto& dependency : original.related) reverse[dependency].push_back(key);
        records.emplace(key, std::move(*row.value));
    }
    for (const auto& entry : manifest.segments) {
        const auto& segment = entry.segment;
        for (std::size_t i = 0; i < segment.blocks.size(); ++i) {
            if (i >= entry.readable.size() || !entry.readable[i]) continue;
            const auto& block = segment.blocks[i];
            const BlockRef ref{segment.key, i};
            native[{segment.key.source, segment.key.session, block.frame, block.row}].push_back(ref);
            if (!block.trigger.empty()) triggers[block.trigger].push_back(ref);
        }
    }
}

}  // namespace waste::m09
