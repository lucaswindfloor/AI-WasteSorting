#pragma once

#include "../archive_service.h"
#include "../storage/command_journal.h"
#include "m07_execution/intent_store_port.h"

namespace waste::m09 {

RecordKey execution_attempt_key(const m06::AttemptId& attempt);
RecordKey execution_intent_key(const m07::ControllerIdentity& controller, const m07::RequestId& request);

class M07IntentStore final : public m07::IntentStorePort {
public:
    M07IntentStore(ArchiveService& archive, std::string station, std::string run);
    m07::LoadReply load() override;
    m07::CommitReply commit(std::uint64_t expected, const m07::DurableState& next) override;

    Result<m07::Intent> lookup(const m07::ControllerIdentity& controller, const m07::RequestId& request) const;
    // Run outside the command submission path. Repeating a revision is safe.
    Result<Manifest> publish(std::uint64_t revision);

private:
    ArchiveService& archive_;
    std::string run_, station_;
    CommandJournal journal_;

    Result<m07::DurableState> checkpoint(std::uint64_t revision = 0) const;
};

}  // namespace waste::m09
