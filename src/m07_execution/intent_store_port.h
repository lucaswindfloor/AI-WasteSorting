#pragma once

#include "execution_types.h"

namespace waste::m07 {

enum class LoadStatus { Empty, Restored, Unavailable };
enum class StoreStatus { Confirmed, Conflict, Rejected, Uncertain };

struct LoadReply {
    LoadStatus status{LoadStatus::Unavailable};
    std::optional<DurableState> state;
};

struct CommitReply {
    StoreStatus status{StoreStatus::Uncertain};
    std::uint64_t revision{};
    std::string transaction;
};

class IntentStorePort {
public:
    virtual ~IntentStorePort() = default;
    virtual LoadReply load() = 0;
    // M09 must compare the head revision and commit the complete new
    // checkpoint, including associated intent bytes and evidence, atomically.
    // Confirmed means it is durably readable after restart. An invalid or
    // unconfirmed acknowledgement must never authorize transmission.
    virtual CommitReply commit(std::uint64_t expected_revision, const DurableState& next) = 0;
};

}  // namespace waste::m07
