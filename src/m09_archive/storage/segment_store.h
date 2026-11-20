#pragma once

#include "local_files.h"

namespace waste::m09 {

struct BlockInput {
    Block metadata;
    std::span<const std::byte> bytes;
};

struct SegmentInput {
    SegmentKey key;
    std::uint64_t epoch{};
    std::string format;
    std::vector<BlockInput> blocks;
    std::vector<Gap> gaps;
};

class SegmentStore {
public:
    SegmentStore(disk::Path run, std::uint64_t max_bytes) : root_(std::move(run)), max_bytes_(max_bytes) {}
    Result<SegmentEntry> put(const SegmentInput& input, bool committed);
    Result<Bytes> read(const SegmentEntry& entry, std::uint64_t block) const;
    Result<SegmentEntry> inspect(const disk::Path& directory) const;
    disk::Path directory(const SegmentKey& key) const;
    disk::Path path() const { return root_ / "segments"; }

private:
    disk::Path root_;
    std::uint64_t max_bytes_;
};

}  // namespace waste::m09
