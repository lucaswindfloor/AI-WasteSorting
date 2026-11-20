#include "fixtures.h"
#include "m09_archive/adapters/m01_capture_adapter.h"
#include "m09_archive/adapters/m07_intent_store_adapter.h"
#include "m09_archive/adapters/m08_evidence_adapter.h"
#include "m09_archive/adapters/m08_journal_adapter.h"
#include "m07_execution/fixtures.h"
#include "m08_hmi/fixtures.h"

#include <algorithm>
#include <array>
#include <iostream>

namespace waste::m09::test {
namespace {

using namespace std::chrono_literals;

class Controller final : public m07::ControllerPort {
public:
    explicit Controller(M07IntentStore& store) : store_(store) {}
    std::vector<m07::WireCommand> sent;
    std::vector<m07::ControllerIdentity> queried;

    Bytes encode(const m07::Command& command) override {
        return {std::byte{0x5a}, static_cast<std::byte>(command.request.operation)};
    }

    m07::Transport transmit(const m07::WireCommand& wire) override {
        const auto& command = wire.command;
        const auto restored = store_.lookup(command.controller, command.request);
        check(restored.value && restored.value->wire == wire && restored.value->progress == m07::Progress::Armed,
              "M07 reached controller before its exact intent was restorable");
        sent.push_back(wire);
        return m07::Transport::Submitted;
    }

    m07::QueryReply query(const m07::ControllerIdentity& controller, const m07::RequestId& request) override {
        queried.push_back(controller);
        return {request, controller, m07::QueryState::Missing, {}};
    }

private:
    M07IntentStore& store_;
};

RecordKey ingest_capture(ArchiveService& archive) {
    auto config = std::make_shared<m01::Config>();
    config->run_id = "P0";
    config->generation = 1;
    config->rgb.device_id = "R01";
    config->rgb.session = 2;
    config->rgb.format.encoding = "rgb8";
    config->hsi.device_id = "H01";
    config->hsi.session = 3;
    config->hsi.format.encoding = "uint16";
    const m01::TriggerKey trigger{5, 10, 2147};
    auto packet = [&](m01::Source source, std::uint64_t session, std::uint64_t frame, std::uint32_t row) {
        m01::ObservationHeader header;
        header.id = {source, session, frame, row};
        header.trigger = trigger;
        header.association_verified = true;
        header.encoder_count = 33815;
        header.bytes = {0, 2};
        return m01::Observation{header, 1ms, std::make_shared<const Bytes>(Bytes{std::byte{4}, std::byte{6}})};
    };
    const m01::TriggerRecord pair{{trigger, 33815, 1, {}}, 1ms,
        packet(m01::Source::Rgb, 2, 10, 7872), packet(m01::Source::Hsi, 3, 11, 467),
        m01::Quality::Valid, m01::Quality::Valid};
    const m01::Segment segment{"P0", 1, 0, config, {pair}};
    M01CaptureAdapter capture(archive, "fixture-host");
    const auto saved = capture.ingest(segment);
    check(saved.value && saved.value->segments.size() == 2, "M01 initial sequence or source separation lost");
    const auto key = RecordKey{Kind::Trigger, capture_trigger_id(trigger), 1};
    const auto observed = archive.record(*saved.value, key);
    check(observed.value && observed.value->raw.size() == 2, "trigger did not retain both source references");
    EvidenceIndex index(archive, *saved.value);
    check(index.native.at({"H01", "3", 11, 467}).size() == 1 && index.triggers.at(key.id).size() == 2,
          "native input and trigger could not be found");
    return key;
}

std::pair<RecordKey, RecordKey> exercise_m07(ArchiveService& archive) {
    M07IntentStore journal(archive, "arm-A", "P0");
    Controller controller(journal);
    m07::Tick now{50ms};
    m05::EligibilityService eligibility(m06::test::recipe());
    m06::test::FakeTargets targets;
    targets.add(m06::test::current());
    m07::ExecutionService execution(m07::test::config(), controller, journal, eligibility, targets,
                                    [&] { return now; });
    check(execution.recover() && execution.update_field(m07::test::field()).issue == m07::Issue::None,
          "M07 could not use persistent checkpoint for startup");
    m06::SchedulingService scheduler(m06::test::config(m06::Contract::B), eligibility, [&] { return now; });
    const auto current = targets.read({"P0", 42}, now);
    const std::array candidates{m06::test::queued(eligibility, *execution.snapshot(), *current)};
    const auto picked = scheduler.dispatch(candidates, execution, targets, now);
    check(picked.task, "M06 handoff through real M07 failed");
    const auto& task = *picked.task;
    now = m07::applicable_receipt(task)->open;
    execution.advance();
    execution.advance();
    check(controller.sent.size() == 1, "real M07 did not make exactly one authorized send");
    const auto& command = controller.sent.front().command;
    check(journal.publish(execution.state()->revision).value, "M07 checkpoint could not be linked into evidence");
    auto wrong = command.controller;
    wrong.session = "different-session";
    check(journal.lookup(wrong, command.request).status == Status::Missing, "old request crossed a controller session");

    M07IntentStore restarted_journal(archive, "arm-A", "P0");
    m07::ExecutionService recovered(m07::test::config(m06::Contract::B, "new-writer"), controller, restarted_journal,
                                   eligibility, targets, [&] { return now; });
    check(recovered.recover(), "real M07 restart with raw checkpoint failed");
    recovered.advance();
    check(controller.sent.size() == 1 && !controller.queried.empty() &&
          controller.queried.back() == command.controller, "old command resent or original session not queried");
    return {execution_attempt_key(task.attempt), execution_intent_key(command.controller, command.request)};
}

void exercise_m08_journal(ArchiveService& archive) {
    m08::test::Fixture bench;
    const auto scope = m08::test::scope;
    M08JournalStore journal(archive, scope);
    m08::HmiService hmi(m08::test::settings(), bench.identity, bench.cell, journal, bench.archive, [&] { return bench.now; });
    check(hmi.recover(), "real M08 could not bootstrap persistent journal");
    const auto req = bench.request("start", m08::Action::Start, std::string(m08::test::baseline));
    check(hmi.operations().submit("operator", req).operation, "M08 action not accepted");
    M08JournalStore reopened(archive, scope);
    m08::HmiService after(m08::test::settings(), bench.identity, bench.cell, reopened, bench.archive, [&] { return bench.now; });
    check(after.recover() && after.operations().query("operator", req.key).operation, "M08 action disappeared after restart");
    const auto head = reopened.load(scope);
    check(head.journal && reopened.publish(head.journal->revision).value, "M08 history not projected into evidence");
}

}  // namespace

void archive_integration() {
    TemporaryArchive temp;
    ArchiveService archive(temp.path);
    const auto trigger = ingest_capture(archive);
    const auto [attempt, intent] = exercise_m07(archive);
    auto target = record("P0", Kind::Target, "42", 1);
    target.target = "42";
    target.related.push_back(trigger);
    check(archive.append("P0", {target}).value, "target version could not be joined");
    const EvidenceQuery query(archive, authorize());
    const auto trace = query.trace("reader", "P0", 0, intent, 100);
    check(trace.value && trace.value->raw.size() == 2 &&
          std::ranges::any_of(trace.value->records, [&](const auto& item) { return item.key == attempt; }),
          "request did not trace across M07 and M01 inputs");
    check(query.trace("unknown", "P0", 0, intent, 100).status == Status::Denied, "archive query authorization missing");

    ReconciliationService independent(archive, authorize());
    check(independent.register_pass("auditor", {"P0", "video", "P042", {}, {"fixture-host", 20}, "video-entry"}).value,
          "independent pass not registered");
    Measurement observed;
    observed.run = "P0";
    observed.scope = "video";
    observed.pass = "P042";
    observed.id = "M042";
    observed.kind = MeasurementKind::Destination;
    observed.occurred = observed.received = {"fixture-host", 30};
    observed.attempt = attempt;
    observed.valid = true;
    observed.destination = Destination::Bin;
    observed.bin = "B02";
    observed.evidence = "video-frame";
    check(independent.measure("auditor", observed).value, "independent bin not registered");
    Reconciliation result{"P0", "video", "P042", 1, 0, Match::Matched, Destination::Bin,
                          "B02", "auditor", "confirmed by video", {"fixture-host", 40}, true,
                          {target.key}, {attempt}, {measurement_key("video", "P042", "M042")}, {"video-frame"}};
    const auto current = independent.revise("auditor", result);
    check(current.value, "independent result did not link attempt");
    const auto actual = query.trace("reader", "P0", current.value->version, pass_key("video", "P042"), 100);
    check(actual.value && std::ranges::any_of(actual.value->records, [](const Record& row) {
        return row.key.kind == Kind::Reconciliation && row.actual_bin == "B02";
    }), "pass did not find actual destination");

    ExportService export_service(archive, authorize());
    const m08::Scope scope{"SORT-01", "P0"};
    M08EvidenceAdapter port(archive, export_service, authorize(), scope);
    const auto version = std::to_string(current.value->version);
    const auto page = port.list({scope, version, {}, 10}, "auditor");
    check(page && page->rows.size() == 1 && page->rows.front().actual_bin == "B02", "real M08 archive query incorrect");
    const m08::ExportRequest req{scope, "csv", version, "42", "42", "verify", 1};
    const auto job = port.enqueue(req, "auditor");
    check(job && job->request == req && job->stage == m08::ExportStage::Queued, "M08 typed CSV request failed");
    check(export_service.advance("auditor", scope.run, req.id).value, "CSV export worker failed");
    const auto ready = port.find(scope, req.id, "auditor");
    check(ready && ready->stage == m08::ExportStage::Ready, "CSV didn't become ready");
    const auto csv = port.download(*ready);
    check(csv && csv->find("B02") != std::string::npos, "CSV did not deliver approved evidence");
    exercise_m08_journal(archive);
}

}  // namespace waste::m09::test

int main() {
    using namespace waste::m09::test;
    try {
        segment_recovery();
        command_journal();
        reconciliation();
        export_replay();
        storage_pressure();
        archive_integration();
        std::cout << "M09 archival, recovery, reconciliation, export and integration scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "M09 test failed: " << error.what() << '\n';
        return 1;
    }
}
