#include "retention_backup.h"
#include "manifest_store.h"

#include <algorithm>

namespace waste::m09 {
namespace {

Status copy_verified_file(const disk::Path& source, const disk::Path& destination) {
    const auto original = disk::digest(source);
    if (!original) return Status::Corrupt;
    if (const auto existing = disk::digest(destination)) {
        return *existing == *original ? Status::Duplicate : Status::Conflict;
    }
    if (!disk::ensure(destination.parent_path()) ||
        !disk::publish(destination, [&](const Sink& write) { return disk::stream(source, write); })) {
        return Status::Unavailable;
    }
    return disk::digest(destination) == original ? Status::Ok : Status::Corrupt;
}

}  // namespace

bool eligible_for_retention_cleanup(const Manifest& manifest, const RetentionState& state) {
    return manifest.sealed && !manifest.reconciliation_pending && !state.unresolved_commands &&
           !state.pinned_readers && !state.held_for_review && state.retention_period_elapsed;
}

Status RetentionBackup::copy(std::string_view subject, const ArchiveService& source, ArchiveService& target,
                             std::string_view run, std::uint64_t version) const {
    if (!version || source.root() == target.root()) return Status::Invalid;
    const Selection all{{}, {}, true};
    const auto approved = authorize_ ? authorize_(subject, run, Access::Export, "archive backup or restore", all)
                                     : std::nullopt;
    if (!approved || *approved != all) return Status::Denied;
    const auto snapshot = source.manifest(run, version);
    if (!snapshot.value) return snapshot.status;
    const auto& manifest = *snapshot.value;
    const auto destination = target.run_path(run);
    if (!disk::ensure(destination)) return Status::Unavailable;
    const disk::Lock lock(destination);
    if (!lock) return Status::Unavailable;
    const ManifestStore target_manifests(destination, std::string(run));
    const auto top = target_manifests.load();
    if (top.value) {
        if (*top.value != manifest) return Status::Conflict;
    } else if (top.status != Status::Missing) {
        return top.status;
    }

    const SegmentStore original(source.run_path(run), 0);
    const SegmentStore replica(destination, 0);
    for (const auto& row : manifest.segments) {
        const auto before = original.directory(row.segment.key);
        const auto after = replica.directory(row.segment.key);
        for (std::size_t i = 0; i < row.readable.size(); ++i) {
            if (row.readable[i] && !source.raw(manifest, {row.segment.key, i}).value) return Status::Corrupt;
        }
        const auto metadata = copy_verified_file(before / "segment", after / "segment");
        if (!accepted(metadata)) return metadata;
        if (std::ranges::any_of(row.readable, [](bool valid) { return valid; })) {
            const auto payload = copy_verified_file(before / "payload", after / "payload");
            if (!accepted(payload)) return payload;
        }
    }
    for (const auto& id : manifest.records) {
        const auto record = source.record(manifest, id);
        if (!record.value) return record.status;
        const auto status = copy_verified_file(source.record_path(run, id), target.record_path(run, id));
        if (!accepted(status)) return status;
    }
    return top.value ? Status::Duplicate : target_manifests.commit(manifest);
}

}  // namespace waste::m09
