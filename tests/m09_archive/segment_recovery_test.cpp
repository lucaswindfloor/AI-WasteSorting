#include "fixtures.h"
#include "m09_archive/storage/manifest_store.h"
#include "m09_archive/storage/record_codec.h"

namespace waste::m09::test {

void segment_recovery() {
    TemporaryArchive temp;
    ArchiveService archive(temp.path);
    const Bytes first{std::byte{1}, std::byte{2}}, last{std::byte{3}, std::byte{4}};
    const auto input = segment("RUN", "set1", first, last);
    const auto manifest = archive.ingest(input);
    check(manifest.status == Status::Ok && manifest.value->segments.size() == 1, "segment not committed");
    const auto entry = manifest.value->segments.front();
    check(entry.segment.blocks[1].bytes == Range{2, 4} && entry.segment.gaps.front().range == Range{467, 468},
          "packed bytes confused with source gaps");
    check(archive.ingest(input).status == Status::Duplicate, "same segment not idempotent");
    auto altered = input;
    altered.gaps.front().reason = "conflicting metadata";
    check(archive.ingest(altered).status == Status::Conflict, "metadata conflict not detected");

    const auto path = SegmentStore(archive.run_path("RUN"), 1024).directory(input.key);
    std::filesystem::resize_file(path / "payload", 3);
    ArchiveService reopened(temp.path);
    check(reopened.raw(*manifest.value, {input.key, 1}).status == Status::Corrupt, "truncated block read succeeded");
    const auto recovered = reopened.recover("RUN");
    check(recovered.value && recovered.value->version > manifest.value->version &&
          recovered.value->segments.front().readable == std::vector<bool>{true, false},
          "recovery lost verifiable prefix or promoted damaged bytes");
    check(reopened.raw(*recovered.value, {input.key, 0}).value == first &&
          reopened.raw(*recovered.value, {input.key, 1}).status == Status::Missing, "recovered coverage wrong");
    check(reopened.manifest("RUN", manifest.value->version).value == manifest.value, "historical manifest overwritten");

    const auto uncommitted = segment("RUN", "between-file-and-manifest", first, last);
    SegmentStore direct(reopened.run_path("RUN"), 1024);
    check(direct.put(uncommitted, false).value, "could not stage orphan segment");
    const auto orphan_path = direct.directory(uncommitted.key);
    std::filesystem::rename(orphan_path / "payload", orphan_path / "payload.pending");
    std::filesystem::resize_file(orphan_path / "payload.pending", 2);
    check(reopened.manifest("RUN").value->segments.size() == 1, "orphan was already visible");
    const auto rebuilt = reopened.recover("RUN");
    check(rebuilt.value && rebuilt.value->segments.size() == 2 &&
          reopened.raw(*rebuilt.value, {uncommitted.key, 0}).value == first, "pending blocks not reconciled");

    const auto head_path = ManifestStore(reopened.run_path("RUN"), "RUN").file(rebuilt.value->version);
    std::filesystem::resize_file(head_path, 1);
    check(reopened.manifest("RUN").status == Status::Corrupt, "normal read silently ignored a damaged manifest");
    const auto repaired = reopened.recover("RUN");
    check(repaired.value && repaired.value->version > rebuilt.value->version && !repaired.value->gaps.empty(),
          "manifest recovery failed to disclose missing version");
}

}  // namespace waste::m09::test
