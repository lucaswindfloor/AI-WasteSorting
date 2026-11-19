#pragma once

#include "scheduling_types.h"

namespace waste::m06 {

struct ReservationRequest {
    AttemptId attempt;
    m05::TargetBinding target;
    std::uint64_t expected_revision{};
    std::vector<ResourceUse> uses;
    Tick handoff_at{}, handoff_deadline{};
};

enum class Reply { Accepted, Conflict, Rejected, Uncertain };
enum class AttemptStatus { Unknown, Held, Delivered, Active, ClosedUnused, ClosedFinished };

struct ReserveReply {
    Reply status{Reply::Uncertain};
    std::optional<Reservation> reservation;
};

class ExecutionPort {
public:
    virtual ~ExecutionPort() = default;
    virtual std::optional<ExecutionSnapshot> snapshot() = 0;
    // Atomic check of the whole resource set, current revision and one current
    // attempt per run/target. Already serviced targets remain blocked until
    // the authoritative state establishes that a new attempt is admissible.
    // Repeating the same request key is idempotent. Conflict/Rejected confirm
    // that no hold was created. Uncertain never authorizes a second attempt.
    virtual ReserveReply reserve(const ReservationRequest& request) = 0;
    // Idempotently accept responsibility for one attempt. Verify the handoff
    // revision, the reservation and the controller contract before accepting.
    // The job may be accepted in advance, with its first budgeted phase
    // anchored to handoff_at and the same held occupancy intervals.
    virtual Reply handoff(const Task& task) = 0;
    // Accepted means positively confirmed unused and closed. Where an action
    // could have escaped, preserve occupancy and report Uncertain.
    virtual Reply close_unused(const AttemptId& attempt, std::string_view token) = 0;
    virtual AttemptStatus status(const AttemptId& attempt) = 0;
};

class TargetSource {
public:
    virtual ~TargetSource() = default;
    virtual std::shared_ptr<const CurrentTarget> read(const TargetKey& target, Tick now) = 0;
};

}  // namespace waste::m06
