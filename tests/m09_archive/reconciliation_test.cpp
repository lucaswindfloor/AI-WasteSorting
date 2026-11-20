#include "fixtures.h"

namespace waste::m09::test {

void reconciliation() {
    TemporaryArchive temp;
    ArchiveService archive(temp.path);
    ReconciliationService measurement(archive, authorize());
    auto target = record("RUN", Kind::Target, "T042");
    target.target = "T042";
    auto attempt = record("RUN", Kind::Attempt, "A042");
    attempt.target = "T042";
    attempt.planned_bin = "B01";
    attempt.related = {target.key};
    check(archive.append("RUN", {target, attempt}).value, "could not write target evidence");
    const MaterialPass pass{"RUN", "bench", "P-1", "bottle", {"test-clock", 101}, "camera entry"};
    auto second = pass;
    second.id = "P-2";
    check(measurement.register_pass("auditor", pass).value && measurement.register_pass("auditor", second).value,
          "reintroduced sample did not get two pass IDs");
    const auto sealed = archive.seal("RUN", true, "Physical process ended");
    check(sealed.value && sealed.value->reconciliation_pending, "seal is distinct from reconciliation complete");

    Reconciliation known;
    known.run = "RUN";
    known.scope = pass.scope;
    known.pass = pass.id;
    known.version = 1;
    known.match = Match::Matched;
    known.destination = Destination::Bin;
    known.actual_bin = "B02";
    known.reviewer = "auditor";
    known.reason = "independent video";
    known.reviewed = {"test-clock", 151};
    known.targets = {target.key};
    known.attempts = {attempt.key};
    known.evidence = {"camera result"};
    check(measurement.revise("auditor", known).status == Status::Invalid, "unsupported actual destination accepted");

    Measurement outcome;
    outcome.run = "RUN";
    outcome.scope = pass.scope;
    outcome.pass = pass.id;
    outcome.id = "D1";
    outcome.kind = MeasurementKind::Destination;
    outcome.occurred = {"test-clock", 142};
    outcome.received = {"test-clock", 150};
    outcome.attempt = attempt.key;
    outcome.valid = true;
    outcome.destination = Destination::Bin;
    outcome.bin = "B02";
    outcome.evidence = "camera result";
    check(measurement.measure("auditor", outcome).value, "actual bin evidence not saved");
    known.measurements = {measurement_key(pass.scope, pass.id, outcome.id)};
    const auto completed = measurement.revise("auditor", known);
    check(completed.value && completed.value->sealed, "supplement not applied to sealed run");
    check(measurement.revise("auditor", known).status == Status::Duplicate, "identical reconciliation not idempotent");
    check(measurement.revise("reader", known).status == Status::Denied, "read-only user edited evidence");
    const auto rows = batch_rows(archive, *completed.value, {0, completed.value->records.size()}, 10);
    check(rows.size() == 2 && rows[0].planned == "B01" && rows[0].actual == "B02" &&
          rows[1].target.empty(), "original plan or unmatched second pass lost");
    const auto before = batch_rows(archive, *sealed.value, {0, sealed.value->records.size()}, 10);
    check(before.size() == 3 && before[1].actual.empty(), "sealed historical snapshot changed");

    auto miss = known;
    miss.pass = second.id;
    miss.match = Match::VerifiedMiss;
    miss.targets.clear();
    miss.attempts.clear();
    miss.measurements.clear();
    miss.destination = Destination::Unknown;
    miss.actual_bin.clear();
    check(measurement.revise("auditor", miss).status == Status::Invalid, "coverage-free detection failure counted");
    miss.coverage_verified = true;
    check(measurement.revise("auditor", miss).value, "verified missed detection not retained");

    auto later = known;
    later.version = 2;
    later.previous = 1;
    later.reason = "annotation reviewed";
    later.reviewed.ticks = 190;
    check(measurement.revise("auditor", later).value, "append-only correction failed");
    later.reason = "competing correction";
    check(measurement.revise("auditor", later).status == Status::Conflict, "competing correction overwrote history");
}

}  // namespace waste::m09::test
