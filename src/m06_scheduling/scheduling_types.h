#pragma once

#include "m05_eligibility/eligibility_service.h"

namespace waste::m06 {

using Tick = m05::Tick;

struct Window {
    Tick open{}, close{}; // Inclusive instants.
    bool operator==(const Window&) const = default;
};

struct Interval {
    Tick begin{}, end{}; // Half-open resource occupancy.
    bool operator==(const Interval&) const = default;
};

enum class Reason {
    None, InvalidInput, WrongGeneration, Capacity, CandidateStale, PredictionInvalid,
    MotionInvalid, NoContactWindow, EarlyContact, LateContact, ReceiveDeadline,
    PlanMismatch, PlanExpired, SnapshotUnavailable, ResourceUnavailable, NoResourceSlot,
    ActiveAttempt, ReservationConflict, ReservationRejected, ReservationExpired,
    ReservationMismatch, RecheckFailed, HandoffRejected, EffectUncertain,
    RemeasurementUnavailable, RemeasurementMismatch, RemeasurementExpired
};

enum class Contract { A, B };
enum class TrackingMode { ControllerNative, ExternalReference };
enum class Receipt { CommitT7, FinalGoalT4 };

struct MotionForecast {
    m03::Generation generation;
    m04::Reference reference;
    // Bounds on forward B.x motion, verified over the stated horizon.
    double min_mm_per_second{}, max_mm_per_second{};
    Tick valid_until{};
    std::string version;
};

struct ContactWindow {
    Window instants;
    m04::Point current_pick;
    m04::Reference current_reference;
    double position_error_mm{};
    std::string region_id;
};

struct Deadlines {
    Receipt receipt{};
    Window receive;
    Window handoff;
    Tick receive_to_contact_min{}, receive_to_contact_max{}, early_margin{}, late_margin{};
    Tick handoff_to_receipt_min{}, handoff_to_receipt_max{};
};

enum class ResourceKind { Robot, Tool, Bin, E11 };

struct ResourceKey {
    ResourceKind kind{};
    std::string id;
    auto operator<=>(const ResourceKey&) const = default;
};

struct ResourceUse {
    ResourceKey resource;
    Interval time;
    bool operator==(const ResourceUse&) const = default;
};

struct OffsetUse {
    ResourceKey resource;
    Interval offset;
};

struct TargetKey {
    std::string run_id;
    m04::TargetId id{};
    auto operator<=>(const TargetKey&) const = default;
};

inline TargetKey target_key(const m05::TargetBinding& t) { return {t.generation.run_id, t.id}; }

struct AttemptId {
    TargetKey target;
    std::string scheduler_session;
    std::uint64_t sequence{};
    auto operator<=>(const AttemptId&) const = default;
};

struct Reservation {
    AttemptId attempt;
    std::string token;
    std::vector<ResourceUse> uses;
    // Latest exclusive time to hand off the held task. Actual occupancy
    // after handoff follows authoritative M07 feedback.
    Tick valid_until{};
    bool operator==(const Reservation&) const = default;
};

struct ResourceCalendar {
    ResourceKey resource;
    Tick valid_until{};
    // The authority reports the intervals supported by current field facts.
    std::vector<Interval> usable;
    std::vector<Interval> occupied;
};

struct ExecutionSnapshot {
    m05::Snapshot eligibility;
    std::vector<ResourceCalendar> calendars;
    std::vector<Reservation> reservations;
    std::vector<TargetKey> unresolved_targets;
};

struct Task {
    AttemptId attempt;
    Reservation reservation;
    m05::Candidate candidate;
    std::string schedule_version, forecast_version;
    TrackingMode tracking{};
    Contract contract{};
    m04::Point pick;
    m04::Reference reference;
    double applied_mm{};
    ContactWindow contact;
    Deadlines deadlines;
    Tick handoff_at{};
    std::uint64_t expected_revision{};
};

struct PlanEnvelope {
    AttemptId attempt;
    m05::TargetBinding target;
    std::string plan_id, reservation_token;
    // Exclusive limit on the actual receipt of COMMIT.
    Tick valid_for_receipt_until{};
    Window expected_contact;
};

struct QueuedCandidate {
    m05::Candidate candidate;
    int priority{};
    std::uint64_t arrival_sequence{};
};

struct CurrentTarget {
    m04::Target target;
    m04::Prediction prediction;
    MotionForecast forecast;
    std::vector<m05::SurfaceGrid> grids;
    std::vector<m05::Region> regions;
};

struct Decision {
    TargetKey target;
    Reason reason{Reason::None};
    std::optional<AttemptId> attempt;
    std::optional<ContactWindow> contact;
    std::optional<Deadlines> deadlines;
};

struct DispatchReport {
    std::optional<Task> task;
    std::vector<Decision> decisions;
};

std::optional<Tick> add_ticks(Tick a, Tick b);
bool valid(Window window);
bool valid(Interval interval);
std::optional<Window> intersection(Window a, Window b);

}  // namespace waste::m06
