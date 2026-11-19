#pragma once

#include "execution_types.h"

namespace waste::m07 {

enum class Transport { Submitted, DefinitelyNotSent, Uncertain };
enum class QueryState { Unknown, Missing, Queued, Acting, Finished, TerminatedWithoutMotion };

struct QueryReply {
    RequestId request;
    ControllerIdentity original;
    QueryState state{QueryState::Unknown};
    // Normalized by the adapter only when it can authenticate the original
    // request/session and prove the claimed transition.
    std::optional<Stamp> proof;
};

class ControllerPort {
public:
    virtual ~ControllerPort() = default;
    // Pure serialization with the approved ICD. The bytes are subsequently
    // persisted unchanged and transmitted with the same request identity.
    virtual std::vector<std::byte> encode(const Command& command) = 0;
    virtual Transport transmit(const WireCommand& command) = 0;
    virtual QueryReply query(const ControllerIdentity& original, const RequestId& request) = 0;
};

}  // namespace waste::m07
