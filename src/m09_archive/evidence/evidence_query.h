#pragma once

#include "evidence_index.h"

namespace waste::m09 {

bool valid_selection(const Selection& selection);
bool subset(const Selection& approved, const Selection& requested);
bool includes_source(const Selection& selection, std::string_view source);
Range record_span(const Selection& selection, std::uint64_t size);

struct Trace {
    std::uint64_t manifest{};
    std::vector<Record> records;
    std::vector<BlockRef> raw;
    std::vector<RecordKey> pending;
    std::vector<Gap> gaps;
    bool truncated{};
};

class EvidenceQuery {
public:
    EvidenceQuery(const ArchiveService& archive, Authorize authorize)
        : archive_(archive), authorize_(std::move(authorize)) {}
    Result<Trace> trace(std::string_view subject, std::string_view run, std::uint64_t version,
                        const RecordKey& start, std::size_t limit) const;
    Result<Trace> target(std::string_view subject, std::string_view run, std::uint64_t version,
                         std::string_view target, std::size_t limit) const;
    Result<Bytes> raw(std::string_view subject, std::uint64_t version, const BlockRef& ref) const;

private:
    const ArchiveService& archive_;
    Authorize authorize_;

    Result<Trace> lookup(std::string_view subject, std::string_view run, std::uint64_t version,
                         const RecordKey* start, std::string_view target, std::size_t limit) const;
};

}  // namespace waste::m09
