#include "archive_service.h"
#include "storage/archive_recovery.h"
#include "storage/manifest_store.h"
#include "storage/record_codec.h"

#include <algorithm>
#include <limits>

namespace waste::m09 {
namespace {

constexpr std::string_view record_schema = "evidence-record-v1";

Manifest starting(std::string_view run) {
    Manifest next;
    next.run = run;
    return next;
}

Result<Manifest> update(const ManifestStore& store, Manifest next, std::string reason = {}) {
    if (next.version == std::numeric_limits<std::uint64_t>::max()) return {Status::Capacity, {}};
    next.previous = next.version++;
    next.reason = std::move(reason);
    const auto result = store.commit(next);
    return {result, result == Status::Ok ? std::optional(std::move(next)) : std::nullopt};
}

bool valid_gap(const Gap& gap) {
    return wire::identifier(gap.source) && wire::identifier(gap.session) && wire::identifier(gap.basis) &&
           wire::identifier(gap.reason) && gap.range.begin < gap.range.end;
}

bool valid_record(const Record& record, std::string_view run) {
    return record.run == run && valid_record_key(record.key) &&
           record.detail.size() <= disk::metadata_limit / 2 &&
           record.related.size() <= 65536 && record.raw.size() <= 65536 &&
           std::ranges::all_of(record.related, valid_record_key) &&
           std::ranges::all_of(record.raw, [&](const BlockRef& ref) { return ref.segment.run == run; });
}

}  // namespace

bool valid_record_key(const RecordKey& key) {
    return key.kind >= Kind::Configuration && key.kind <= Kind::Reconciliation &&
           wire::identifier(key.id) && key.version;
}

ArchiveService::ArchiveService(disk::Path root, Limits limits)
    : root_(std::filesystem::absolute(root).lexically_normal()), limits_(limits) {
    if (!limits_.segment_bytes || !limits_.index_entries || limits_.index_entries > 1000000 ||
        !disk::ensure(root_ / "runs")) throw std::invalid_argument("invalid M09 archive root or limits");
}

disk::Path ArchiveService::run_path(std::string_view run) const {
    return root_ / "runs" / wire::key({run});
}

disk::Path ArchiveService::record_path(std::string_view run, const RecordKey& key) const {
    return run_path(run) / "records" / wire::key({std::to_string(static_cast<unsigned>(key.kind)),
                                               key.id, std::to_string(key.version)});
}

Result<Manifest> ArchiveService::manifest(std::string_view run, std::uint64_t version) const {
    if (!wire::identifier(run)) return {Status::Invalid, {}};
    const auto path = run_path(run);
    std::error_code error;
    if (!std::filesystem::is_directory(path, error)) {
        return {error && error != std::errc::no_such_file_or_directory ? Status::Unavailable : Status::Missing, {}};
    }
    const disk::Lock lock(path);
    if (!lock) return {};
    return ManifestStore(path, std::string(run)).load(version);
}

Result<Manifest> ArchiveService::ingest(const SegmentInput& input) {
    if (!wire::identifier(input.key.run)) return {Status::Invalid, {}};
    const auto path = run_path(input.key.run);
    if (!disk::ensure(path)) return {};
    const disk::Lock lock(path);
    if (!lock) return {};
    const ManifestStore manifests(path, input.key.run);
    auto current = manifests.load();
    if (!current.value && current.status != Status::Missing) return current;
    auto next = current.value.value_or(starting(input.key.run));
    const auto found = std::ranges::find_if(next.segments, [&](const auto& entry) {
        return entry.segment.key == input.key;
    });
    if (found == next.segments.end()) {
        if (next.segments.size() >= limits_.index_entries) return {Status::Capacity, {}};
        const auto free = pressure(0, 0, 0, std::numeric_limits<std::uint64_t>::max()).usable_disk;
        std::uint64_t required = 0;
        for (const auto& block : input.blocks) {
            if (block.bytes.size() > free - required) return {Status::Capacity, {}};
            required += block.bytes.size();
        }
        if (required == free) return {Status::Capacity, {}};
    }
    SegmentStore segments(path, limits_.segment_bytes);
    auto stored = segments.put(input, found != next.segments.end());
    if (!stored.value) return {stored.status, {}};
    if (found != next.segments.end()) return {Status::Duplicate, std::move(next)};
    next.segments.push_back(std::move(*stored.value));
    const auto reason = next.sealed ? "Supplemental source evidence" : "Source segment committed";
    return update(manifests, std::move(next), reason);
}

Result<Manifest> ArchiveService::append(std::string_view run, const std::vector<Record>& records,
                                       std::optional<std::uint64_t> expected) {
    if (!wire::identifier(run) || records.empty() ||
        !std::ranges::all_of(records, [&](const auto& record) { return valid_record(record, run); })) {
        return {Status::Invalid, {}};
    }
    const auto path = run_path(run);
    if (!disk::ensure(path)) return {};
    const disk::Lock lock(path);
    if (!lock) return {};
    const ManifestStore manifests(path, std::string(run));
    auto current = manifests.load();
    if (!current.value && current.status != Status::Missing) return current;
    auto next = current.value.value_or(starting(run));
    if (expected && *expected != next.version) return {Status::Conflict, {}};
    const auto previous_count = next.records.size();
    std::map<RecordKey, Bytes> pending;
    for (const auto& row : records) {
        const auto previous = disk::read(record_path(run, row.key));
        auto encoded = wire::encode(record_schema, row);
        if (encoded.size() > disk::metadata_limit) return {Status::Capacity, {}};
        if (previous.value) {
            if (*previous.value != encoded) return {Status::Conflict, {}};
        } else if (previous.status != Status::Missing) {
            return {previous.status, {}};
        } else {
            const auto [it, inserted] = pending.try_emplace(row.key, std::move(encoded));
            if (!inserted && it->second != encoded) return {Status::Conflict, {}};
        }
        if (std::ranges::find(next.records, row.key) == next.records.end()) next.records.push_back(row.key);
    }
    if (next.records.size() == previous_count) return {Status::Duplicate, std::move(next)};
    if (next.records.size() > limits_.index_entries) return {Status::Capacity, {}};
    const auto objects = path / "records";
    if (!disk::ensure(objects)) return {};
    for (const auto& [key, bytes] : pending) {
        if (!disk::publish(record_path(run, key), bytes)) return {};
    }
    if (!disk::sync(objects)) return {};
    const auto reason = next.sealed ? "Supplemental records" : "Evidence records committed";
    return update(manifests, std::move(next), reason);
}

Result<Manifest> ArchiveService::gap(std::string_view run, const Gap& missing) {
    if (!wire::identifier(run) || !valid_gap(missing)) return {Status::Invalid, {}};
    const auto path = run_path(run);
    if (!disk::ensure(path)) return {};
    const disk::Lock lock(path);
    if (!lock) return {};
    const ManifestStore manifests(path, std::string(run));
    auto current = manifests.load();
    if (!current.value && current.status != Status::Missing) return current;
    auto next = current.value.value_or(starting(run));
    if (std::ranges::find(next.gaps, missing) != next.gaps.end()) return {Status::Duplicate, std::move(next)};
    if (next.gaps.size() >= limits_.index_entries) return {Status::Capacity, {}};
    next.gaps.push_back(missing);
    return update(manifests, std::move(next), "Source gap reported");
}

Result<Manifest> ArchiveService::seal(std::string_view run, bool reconciliation_pending, std::string reason) {
    if (!wire::identifier(run) || !wire::identifier(reason)) return {Status::Invalid, {}};
    const auto path = run_path(run);
    const disk::Lock lock(path);
    if (!lock) return {};
    const ManifestStore manifests(path, std::string(run));
    auto current = manifests.load();
    if (!current.value) return current;
    if (current.value->sealed && current.value->reconciliation_pending == reconciliation_pending) {
        current.status = Status::Duplicate;
        return current;
    }
    current.value->sealed = true;
    current.value->reconciliation_pending = reconciliation_pending;
    return update(manifests, std::move(*current.value), std::move(reason));
}

Result<Manifest> ArchiveService::recover(std::string_view run) {
    if (!wire::identifier(run)) return {Status::Invalid, {}};
    const auto path = run_path(run);
    const disk::Lock lock(path);
    if (!lock) return {};
    return recover_archive(path, std::string(run), limits_.segment_bytes, limits_.index_entries);
}

Result<Record> ArchiveService::record(const Manifest& manifest, const RecordKey& key) const {
    if (std::ranges::find(manifest.records, key) == manifest.records.end()) return {Status::Missing, {}};
    const auto bytes = disk::read(record_path(manifest.run, key));
    if (!bytes.value) return {bytes.status == Status::Missing ? Status::Corrupt : bytes.status, {}};
    auto row = wire::decode<Record>(record_schema, *bytes.value);
    if (!row || !valid_record(*row, manifest.run) || row->key != key) return {Status::Corrupt, {}};
    return {Status::Ok, std::move(row)};
}

Result<Bytes> ArchiveService::raw(const Manifest& manifest, const BlockRef& ref) const {
    if (ref.segment.run != manifest.run) return {Status::Invalid, {}};
    const auto entry = std::ranges::find(manifest.segments, ref.segment, [](const auto& row) {
        return row.segment.key;
    });
    if (entry == manifest.segments.end()) return {Status::Missing, {}};
    return SegmentStore(run_path(manifest.run), limits_.segment_bytes).read(*entry, ref.index);
}

Pressure ArchiveService::pressure(std::uint64_t queued, std::uint64_t in_flight, std::uint64_t tail,
                                  std::uint64_t buffer_free) const {
    std::error_code error;
    const auto free = std::filesystem::space(root_, error);
    return assess_pressure({error ? 0 : free.available, limits_.command_reserve, queued, in_flight, tail, buffer_free});
}

}  // namespace waste::m09
