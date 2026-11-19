#pragma once

#include "../m06_scheduling/fake_execution.h"
#include "m07_execution/execution_service.h"

namespace waste::m07::test {

using namespace std::chrono_literals;
using m02::test::check;

inline ExecutionConfig config(m06::Contract contract = m06::Contract::B, std::string writer = "exec-1") {
    return {{"P0", 1}, "execution-1", std::move(writer), "PLC", "P02",
            500ms, 350ms, 100ms, 5ms, 24, 48, 1024, 32, 256,
            {{"arm-A", "R1", "E01", "tool-feed", contract, m06::TrackingMode::ControllerNative,
              2ms, contract == m06::Contract::A}}};
}

inline FieldState field(std::uint64_t sequence = 1, Tick observed = 50ms) {
    auto source = m06::test::snapshot();
    return {std::move(source.eligibility), std::move(source.calendars),
            {{"E01", "session-1", observed, 490ms, true}},
            {{"tool-feed", "terminal-1", observed, 490ms, true}},
            {"PLC", observed, 490ms, true}, {"P02", observed, 490ms, true}, sequence, observed};
}

class MemoryStore final : public IntentStorePort {
public:
    std::optional<DurableState> saved;
    bool unavailable{}, reject_once{}, fake_ack_once{}, write_without_ack_once{};
    std::size_t writes{};

    LoadReply load() override {
        return unavailable ? LoadReply{} : saved ? LoadReply{LoadStatus::Restored, saved}
                                                 : LoadReply{LoadStatus::Empty, {}};
    }

    CommitReply commit(std::uint64_t expected, const DurableState& next) override {
        ++writes;
        if ((saved ? saved->revision : 0) != expected) {
            return {StoreStatus::Conflict, 0, {}};
        }
        if (reject_once) {
            reject_once = false;
            return {StoreStatus::Rejected, 0, {}};
        }
        if (fake_ack_once) {
            fake_ack_once = false;
            return {StoreStatus::Confirmed, next.revision, "ack-for-different-record"};
        }
        saved = next;
        if (write_without_ack_once) {
            write_without_ack_once = false;
            return {};
        }
        return {StoreStatus::Confirmed, next.revision, next.transaction};
    }
};

class FakeController final : public ControllerPort {
public:
    explicit FakeController(MemoryStore& store) : store_(store) {}
    std::vector<WireCommand> sent;
    std::vector<std::pair<ControllerIdentity, RequestId>> queries;
    QueryState query_state{QueryState::Missing};
    std::optional<Stamp> query_proof;
    Transport outcome{Transport::Submitted};
    bool lose_send_checkpoint_once{};

    std::vector<std::byte> encode(const Command& command) override {
        return {std::byte{0x01}, static_cast<std::byte>(command.request.operation)};
    }
    Transport transmit(const WireCommand& command) override {
        const auto& record = store_.saved->attempts.at(command.command.request.attempt);
        check(record.intents.at(command.command.request.operation).wire == command &&
              record.intents.at(command.command.request.operation).progress == Progress::Armed,
              "controller got bytes without matching recoverable intent");
        sent.push_back(command);
        if (lose_send_checkpoint_once) {
            store_.reject_once = true;
            lose_send_checkpoint_once = false;
        }
        return outcome;
    }
    QueryReply query(const ControllerIdentity& original, const RequestId& request) override {
        queries.emplace_back(original, request);
        return {request, original, query_state, query_proof};
    }

private:
    MemoryStore& store_;
};

struct Bench {
    Tick now{50ms};
    MemoryStore store;
    FakeController controller{store};
    m05::EligibilityService eligibility{m06::test::recipe()};
    m06::test::FakeTargets targets;
    ExecutionConfig configuration;
    ExecutionService execution;
    m06::Contract contract;

    explicit Bench(m06::Contract mode = m06::Contract::B)
        : configuration(config(mode)), execution(configuration, controller, store, eligibility, targets,
                                                   [&] { return now; }), contract(mode) {
        targets.add(m06::test::current());
        check(execution.recover(), "initial open failed");
        check(execution.update_field(field()).issue == Issue::None, "field not published");
    }

    m06::Task schedule() {
        m06::SchedulingService scheduler(m06::test::config(contract), eligibility, [&] { return now; });
        const auto snapshot = execution.snapshot();
        const auto target = targets.read({"P0", 42}, now);
        const std::array candidates{m06::test::queued(eligibility, *snapshot, *target)};
        const auto result = scheduler.dispatch(candidates, execution, targets, now);
        check(result.task.has_value(), "M06 did not hand off to real M07");
        return *result.task;
    }

    void advance_at(Tick value, int cycles = 1) {
        now = value;
        for (int i = 0; i < cycles; ++i) {
            execution.advance();
        }
    }
};

inline Feedback controller_event(const m06::Task& task, Operation op, EventKind kind,
                                 std::uint64_t sequence, Tick occurred, std::string session = "session-1") {
    return {task.attempt, RequestId{task.attempt, op},
            {Source::Controller, "E01", std::move(session), sequence, occurred, occurred + 90ms, "E01-event"},
            kind, {}};
}

inline Feedback tool_event(const m06::Task& task, EventKind kind, std::uint64_t sequence, Tick occurred) {
    return {task.attempt, {}, {Source::Terminal, "tool-feed", "terminal-1", sequence, occurred,
                               occurred + 90ms, "tool-event"}, kind, {}};
}

void reservation_test();
void command_test();
void reducer_test();
void recovery_test();

}  // namespace waste::m07::test
