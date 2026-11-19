#pragma once

#include "command_dispatch.h"
#include "execution_recovery.h"
#include "intent_store_port.h"

#include <set>

namespace waste::m07 {

// One serialized execution lane owns all operations and callbacks to ports.
class ExecutionService final : public m06::ExecutionPort {
public:
    ExecutionService(ExecutionConfig config, ControllerPort& controller, IntentStorePort& store,
                     const m05::EligibilityService& eligibility, m06::TargetSource& targets, Clock clock);

    bool recover();
    Report update_field(FieldState field);
    Report ingest(const Feedback& feedback);
    Report request_cancel(const m06::AttemptId& attempt);
    std::vector<Report> advance();
    std::optional<DurableState> state() const;

    std::optional<m06::ExecutionSnapshot> snapshot() override;
    m06::ReserveReply reserve(const m06::ReservationRequest& request) override;
    m06::Reply handoff(const m06::Task& task) override;
    m06::Reply close_unused(const m06::AttemptId& attempt, std::string_view token) override;
    m06::AttemptStatus status(const m06::AttemptId& attempt) override;

private:
    Tick time();
    bool commit(DurableState next);
    Issue live_task(const AttemptRecord& attempt, Tick now);
    Report step(const m06::AttemptId& id);
    Report arm(const m06::AttemptId& id, Operation operation);
    Report transmit(const m06::AttemptId& id, Operation operation);
    Report query(const m06::AttemptId& id);
    Report record_issue(const m06::AttemptId& id, Issue issue, bool block);
    Report report(std::optional<m06::AttemptId> attempt, Issue issue,
                  std::optional<RequestId> request = {}) const;

    ExecutionConfig config_;
    ControllerPort& controller_;
    IntentStorePort& store_;
    const m05::EligibilityService& eligibility_;
    m06::TargetSource& targets_;
    Clock clock_;
    DurableState state_;
    bool ready_{};
    Tick now_{};
    std::uint64_t transaction_sequence_{};
    std::set<RequestId> armed_here_;
};

}  // namespace waste::m07
