#pragma once

#include "bin_change_workflow.h"
#include "configuration_service.h"
#include "hmi_ports.h"

namespace waste::m08 {

class OperationService {
public:
    OperationService(Settings settings, IdentityPort& identity, WorkCellPort& cell,
                     JournalPort& store, Clock clock);
    bool recover();
    OperationReply submit(std::string_view credential, const Request& request);
    OperationReply query(std::string_view credential, const OperationKey& key);
    std::optional<Workbench> workbench(std::string_view credential);
    void advance();
    const Settings& settings() const { return settings_; }

private:
    Tick time();
    bool commit(Journal next);
    std::optional<Runtime> live();
    bool authorized_to_send(const Operation& op);
    void dispatch(const OperationKey& key, Step step, const Runtime& runtime, Tick now);
    void advance_one(const OperationKey& key, Tick now);
    OperationReply fail(const Request& request, std::string actor, Issue issue);
    bool set_phase(const OperationKey& key, Phase phase, Issue issue, Tick now);

    Settings settings_;
    IdentityPort& identity_;
    WorkCellPort& cell_;
    JournalPort& store_;
    Clock clock_;
    Journal journal_;
    Tick now_{};
    std::uint64_t serial_{};
    bool ready_{};
};

}  // namespace waste::m08
