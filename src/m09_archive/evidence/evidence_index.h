#pragma once

#include "../archive_service.h"

namespace waste::m09 {

struct NativeKey {
    std::string source, session;
    std::uint64_t frame{}, row{};
    auto operator<=>(const NativeKey&) const = default;
};

class EvidenceIndex {
public:
    EvidenceIndex(const ArchiveService& archive, const Manifest& manifest);
    std::map<RecordKey, Record> records;
    std::map<RecordKey, std::vector<RecordKey>> reverse;
    std::map<std::string, std::vector<RecordKey>> targets;
    std::map<NativeKey, std::vector<BlockRef>> native;
    std::map<std::string, std::vector<BlockRef>> triggers;
    std::vector<RecordKey> unavailable;
};

}  // namespace waste::m09
