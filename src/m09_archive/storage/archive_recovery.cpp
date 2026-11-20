#include "archive_recovery.h"
#include "manifest_store.h"
#include "segment_store.h"

#include <algorithm>
#include <limits>
#include <set>

namespace waste::m09 {

Result<Manifest> recover_archive(const disk::Path& path, const std::string& run,
                                 std::uint64_t max_segment_bytes, std::uint64_t max_entries) {
    const ManifestStore store(path, run);
    const auto versions = store.versions();
    if (!versions.value) return {versions.status, {}};
    const auto high = versions.value->empty() ? 0 : versions.value->back();
    Manifest recovered;
    recovered.run = run;
    bool changed = false;
    for (auto it = versions.value->rbegin(); it != versions.value->rend(); ++it) {
        auto old = store.load(*it);
        if (old.value) {
            recovered = std::move(*old.value);
            break;
        }
        if (old.status != Status::Corrupt) return {old.status, {}};
        changed = true;
    }
    if (recovered.version != high && high > recovered.version && high < UINT64_MAX) {
        recovered.gaps.push_back({"archive", "manifest", "version", {recovered.version + 1, high + 1},
                                  "Unreadable manifest versions"});
    }

    const SegmentStore segments(path, max_segment_bytes);
    std::set<disk::Path> verified;
    std::error_code error;
    std::filesystem::directory_iterator it(segments.path(), error), end;
    if (error == std::errc::no_such_file_or_directory) error.clear();
    for (; !error && it != end; it.increment(error)) {
        if (!std::filesystem::is_directory(it->symlink_status(error))) continue;
        auto report = segments.inspect(it->path());
        if (!report.value) {
            if (report.status == Status::Unavailable) return {};
            continue;
        }
        auto& row = *report.value;
        if (row.segment.key.run != run) continue;
        verified.insert(it->path());
        const auto old = std::ranges::find_if(recovered.segments, [&](const auto& entry) {
            return entry.segment.key == row.segment.key;
        });
        if (old == recovered.segments.end()) {
            if (recovered.segments.size() >= max_entries) return {Status::Capacity, {}};
            recovered.segments.push_back(std::move(row));
            changed = true;
        } else if (*old != row) {
            *old = std::move(row);
            changed = true;
        }
    }
    if (error) return {};
    for (auto& row : recovered.segments) {
        if (verified.contains(segments.directory(row.segment.key))) continue;
        auto damaged = row;
        damaged.readable.assign(row.segment.blocks.size(), false);
        damaged.recovery_gaps = {{row.segment.key.source, row.segment.key.session, "byte",
                                  {0, row.segment.length}, "Segment missing or unverifiable"}};
        if (row != damaged) {
            row = std::move(damaged);
            changed = true;
        }
    }
    if (!changed) {
        return high ? Result<Manifest>{Status::Duplicate, std::move(recovered)}
                    : Result<Manifest>{Status::Missing, {}};
    }
    if (high == UINT64_MAX) return {Status::Capacity, {}};
    recovered.previous = recovered.version;
    recovered.version = high + 1;
    recovered.reason = "Recovery verified the remaining archived ranges";
    const auto result = store.commit(recovered);
    return {result, result == Status::Ok ? std::optional(std::move(recovered)) : std::nullopt};
}

}  // namespace waste::m09
