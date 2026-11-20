#pragma once

#include "../archive_service.h"
#include "../storage/command_journal.h"
#include "m08_hmi/hmi_ports.h"

namespace waste::m09 {

class M08JournalStore final : public m08::JournalPort {
public:
    M08JournalStore(ArchiveService& archive, m08::Scope scope);
    m08::Load load(const m08::Scope& scope) override;
    m08::Confirmation commit(std::uint64_t expected, const m08::Journal& next) override;

    Result<Manifest> publish(std::uint64_t revision);

private:
    ArchiveService& archive_;
    m08::Scope scope_;
    CommandJournal journal_;

    Result<m08::Journal> checkpoint(std::uint64_t revision = 0) const;
};

}  // namespace waste::m09
