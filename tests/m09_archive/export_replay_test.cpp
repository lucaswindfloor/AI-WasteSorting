#include "fixtures.h"
#include "m09_archive/evidence/export_service.h"
#include "m09_archive/evidence/replay_reader.h"
#include "m09_archive/storage/retention_backup.h"

namespace waste::m09::test {

void export_replay() {
    TemporaryArchive temp, replica;
    ArchiveService archive(temp.path), backup(replica.path);
    const Bytes first{std::byte{0x23}}, second{std::byte{0x15}};
    const auto input = segment("RUN", "initial", first, second);
    const auto old = archive.ingest(input);
    check(old.value, "could not create export input");
    auto late = segment("RUN", "late", second, first);
    late.blocks[0].metadata.occurred.ticks = 50;
    late.blocks[0].metadata.received.ticks = 150;
    const auto supplemented = archive.ingest(late);
    check(supplemented.value, "could not add late capture");
    const ReplayReader replay(archive, authorize());
    const Selection all{{}, {}, true};
    const auto original = replay.plan("reader", "RUN", old.value->version, ReplayMode::AsObserved, all);
    const auto received = replay.plan("reader", "RUN", supplemented.value->version, ReplayMode::AsObserved, all);
    const auto recompute = replay.plan("reader", "RUN", supplemented.value->version, ReplayMode::WithSupplements, all);
    check(original.value->items.size() == 2 && received.value->items.back().time.ticks == 150 &&
          recompute.value->items.front().time.ticks == 50 && !original.value->gaps.empty(),
          "arrival order confused with source order or original range changed");

    bool enabled = true;
    const Authorize restrict = [&](std::string_view user, std::string_view, Access access, std::string_view,
                                  const Selection& input) -> std::optional<Selection> {
        if (!enabled || user != "auditor" || access != Access::Export) return {};
        auto allowed = input;
        allowed.sources = {"HSI"};
        return allowed;
    };
    const ExportRequest job{"RUN", "test-export", "auditor", "review", old.value->version, all, 20000,
                            ExportFormat::Package, 0, {}, {}};
    ExportService exporter(archive, restrict, 50000);
    check(exporter.enqueue(job).value && exporter.enqueue(job).status == Status::Duplicate,
          "export retry not idempotent");
    auto conflict = job;
    conflict.purpose = "different scope";
    check(exporter.enqueue(conflict).status == Status::Conflict, "export changed existing job");
    check(exporter.status("reader", "RUN", job.id).status == Status::Missing, "export ownership leaked");
    const auto ready = exporter.advance(job.owner, job.run, job.id);
    check(ready.value && ready.value->stage == ExportStage::Ready && ready.value->approved.sources == std::vector<std::string>{"HSI"},
          "approved subset could not be exported");
    Bytes delivered;
    check(exporter.stream(job.owner, job.run, job.id, [&](auto bytes) {
        delivered.insert(delivered.end(), bytes.begin(), bytes.end());
        return true;
    }) == Status::Ok && delivered.size() == ready.value->bytes, "could not stream verified deliverable");
    check(disk::publish(temp.path / "copy", delivered), "could not materialize authorized package");
    std::size_t replayed = 0;
    const auto verified = read_evidence_package(temp.path / "copy", ready.value->digest, {},
        [&](const BlockRef&, std::span<const std::byte>) { ++replayed; return true; });
    check(verified.value && verified.value->manifest.version == job.manifest && replayed == 2,
          "offline package did not independently verify and expose requested blocks");
    check(read_evidence_package(temp.path / "copy", std::string(64, '0')).status == Status::Corrupt,
          "offline package accepted different checksum");
    enabled = false;
    check(exporter.stream(job.owner, job.run, job.id, [](auto) { return true; }) == Status::Denied,
          "revoked export authorization not checked");
    const RetentionBackup transfer(authorize());
    check(transfer.copy("auditor", archive, backup, "RUN", old.value->version) == Status::Ok &&
          backup.manifest("RUN").value == old.value &&
          backup.raw(*old.value, {input.key, 1}).value == second, "pinned backup or restore failed");
}

}  // namespace waste::m09::test
