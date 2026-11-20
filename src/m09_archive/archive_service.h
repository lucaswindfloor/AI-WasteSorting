#pragma once

#include "archive_types.h"
#include "storage/segment_store.h"
#include "storage/storage_pressure.h"

namespace waste::m09 {

struct Limits {
    std::uint64_t segment_bytes{64 * 1024 * 1024};
    std::uint64_t command_reserve{64 * 1024 * 1024};
    std::uint64_t index_entries{100000};
};

class ArchiveService {
public:
    ArchiveService(disk::Path root, Limits limits = {});

    Result<Manifest> manifest(std::string_view run, std::uint64_t version = 0) const;
    Result<Manifest> ingest(const SegmentInput& input);
    Result<Manifest> append(std::string_view run, const std::vector<Record>& records,
                            std::optional<std::uint64_t> expected = {});
    Result<Manifest> gap(std::string_view run, const Gap& gap);
    Result<Manifest> seal(std::string_view run, bool reconciliation_pending, std::string reason);
    Result<Manifest> recover(std::string_view run);

    Result<Record> record(const Manifest& manifest, const RecordKey& key) const;
    Result<Bytes> raw(const Manifest& manifest, const BlockRef& ref) const;
    Pressure pressure(std::uint64_t queued, std::uint64_t in_flight, std::uint64_t tail,
                      std::uint64_t buffer_free) const;

    disk::Path root() const { return root_; }
    disk::Path run_path(std::string_view run) const;

private:
    friend class RetentionBackup;
    disk::Path root_;
    Limits limits_;

    disk::Path record_path(std::string_view run, const RecordKey& key) const;
};

bool valid_record_key(const RecordKey& key);

}  // namespace waste::m09
