#pragma once

#include "m06_scheduling/execution_port.h"

#include <map>

namespace waste::m07 {

using m06::Tick;
using Clock = std::function<Tick()>;

enum class Operation { Prepare, Commit, FinalGoal, Cancel };
enum class Progress { Armed, Sent, Received, Uncertain, NotSent };
enum class Stage {
    Held, Queued, Preparing, PlanReady, AwaitingReceipt, Received, Active,
    Returning, Pending, ClosedUnused, ClosedFinished
};
enum class Carrier { Unknown, OnBelt, Held, Released };
enum class Issue {
    None, Duplicate, NotReady, StaleRevision, BadIdentity, InvalidInput,
    ResourceConflict, Expired, FieldBlocked, TargetChanged, PlanInvalid,
    PersistenceUnavailable, IdempotencyConflict, TransportUncertain, EvidenceConflict,
    QueryUnresolved, ControllerRejected
};

struct ControllerIdentity {
    std::string device, session;
    auto operator<=>(const ControllerIdentity&) const = default;
};

struct DeviceLink {
    std::string device, session;
    Tick observed{}, valid_until{};
    bool available{};
};

struct Gate {
    std::string source;
    Tick observed{}, valid_until{};
    bool permits{};
};

struct FieldState {
    m05::Snapshot eligibility;
    std::vector<m06::ResourceCalendar> calendars;
    std::vector<DeviceLink> controllers, terminals;
    Gate process;
    // Read-only IF-007 mirror. The core only uses it for business admission.
    Gate safety_mirror;
    std::uint64_t sequence{};
    Tick observed{};
};

struct RequestId {
    m06::AttemptId attempt;
    Operation operation{};
    auto operator<=>(const RequestId&) const = default;
};

struct Payload {
    m05::TargetBinding target;
    std::string reservation_token, schedule_version, forecast_version;
    std::string recipe_version, proof_id, profile_version, region_id, bin_mapping_version;
    m05::ToolIdentity tool;
    m05::BinIdentity bin;
    std::uint64_t placement_revision{};
    m06::Contract contract{};
    m06::TrackingMode tracking{};
    m04::Point pick;
    m04::Reference reference;
    double applied_mm{};
    m06::Window contact, receive, handoff;
    Tick early_margin{}, late_margin{}, receive_to_contact_min{}, receive_to_contact_max{};
    Tick handoff_at{}, valid_until{};
    std::uint64_t expected_revision{};
    std::optional<std::string> appointment_id;
    bool operator==(const Payload&) const = default;
};

struct Command {
    RequestId request;
    ControllerIdentity controller;
    Payload payload;
    std::optional<std::string> plan_id;
    std::optional<RequestId> related_request;
    bool operator==(const Command&) const = default;
};

struct WireCommand {
    Command command;
    std::vector<std::byte> bytes;
    bool operator==(const WireCommand&) const = default;
};

struct Intent {
    WireCommand wire;
    Progress progress{Progress::Armed};
    Tick armed_at{};
    std::optional<Tick> sent_at;
};

enum class Source { Controller, Terminal };

struct Stamp {
    Source source{};
    std::string device, session;
    std::uint64_t sequence{};
    Tick occurred{}, valid_until{};
    std::string evidence;
    bool operator==(const Stamp&) const = default;
};

enum class EventKind {
    Receipt, PlanReady, Admitted, Approaching, MotionObserved, Holding, PlacementEnded,
    Released, Returning, RobotReady, ToolEmpty, ToolHealthy, ToolUnavailable,
    CancelAcknowledged, NoAction, Finished
};

struct Feedback {
    m06::AttemptId attempt;
    // Controller feedback names the original request. Terminal evidence is
    // explicitly correlated to the attempt by the device adapter.
    std::optional<RequestId> request;
    Stamp stamp;
    EventKind kind{};
    std::optional<m06::PlanEnvelope> plan;
};

struct AttemptRecord {
    m06::ReservationRequest original;
    m06::Reservation reservation;
    std::optional<m06::Task> task;
    std::optional<ControllerIdentity> controller;
    Stage stage{Stage::Held};
    Carrier carrier{Carrier::Unknown};
    Issue issue{Issue::None};
    std::map<Operation, Intent> intents;
    std::map<EventKind, Feedback> facts;
    std::vector<Feedback> recent_events;
    std::optional<m06::PlanEnvelope> plan;
    std::optional<Tick> last_query;
    bool cancellation_requested{};
    // Set for authoritative action evidence and retained even if later
    // messages claim no action.
    bool motion_seen{};
    bool needs_query{};
};

struct DurableState {
    m03::Generation generation;
    std::string configuration, transaction;
    std::uint64_t revision{};
    Tick updated{};
    std::optional<FieldState> field;
    std::uint64_t field_sequence{};
    std::vector<Feedback> historical;
    std::map<m06::AttemptId, AttemptRecord> attempts;
};

struct Report {
    std::optional<m06::AttemptId> attempt;
    std::optional<RequestId> request;
    Issue issue{Issue::None};
    std::uint64_t revision{};
};

bool closed(Stage stage);
bool same_reservation_request(const m06::ReservationRequest& a, const m06::ReservationRequest& b);

}  // namespace waste::m07
