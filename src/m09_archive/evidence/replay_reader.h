#pragma once

#include "evidence_query.h"

namespace waste::m09 {

enum class ReplayMode { AsObserved, WithSupplements };

struct ReplayItem {
    Time time;
    std::optional<RecordKey> record;
    std::optional<BlockRef> block;
};

struct ReplayPlan {
    std::string run;
    std::uint64_t manifest{};
    ReplayMode mode{};
    std::vector<ReplayItem> items;
    std::vector<Gap> gaps;
    bool cross_domain_unordered{};
};

struct PackageContents {
    Manifest manifest;
    Selection approved;
};

using ReplayRecordSink = std::function<bool(const Record&)>;
using ReplayBlockSink = std::function<bool(const BlockRef&, std::span<const std::byte>)>;

// The offline recipient obtains the digest from the authorized handoff.
Result<PackageContents> read_evidence_package(const disk::Path& file, std::string_view digest,
                                             const ReplayRecordSink& records = {},
                                             const ReplayBlockSink& blocks = {});

class ReplayReader {
public:
    ReplayReader(const ArchiveService& archive, Authorize authorize)
        : archive_(archive), authorize_(std::move(authorize)) {}

    Result<ReplayPlan> plan(std::string_view subject, std::string_view run, std::uint64_t version,
                            ReplayMode mode, const Selection& selection) const;

private:
    const ArchiveService& archive_;
    Authorize authorize_;
};

}  // namespace waste::m09
