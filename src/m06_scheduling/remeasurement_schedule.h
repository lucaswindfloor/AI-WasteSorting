#pragma once

#include "execution_port.h"
#include "resource_schedule.h"

namespace waste::m06 {

struct Exposure {
    std::string component;
    Interval offset; // From the first exposure.
    std::vector<m05::MissingComponent> provides{};
};

struct RemeasurementBudget {
    std::string camera;
    std::vector<Exposure> exposures;
    Tick first_to_result_upper{}, verification_and_recheck_upper{}, camera_busy_upper{};
};

struct ExposureWindow {
    std::string component;
    std::vector<Interval> visibility;
};

struct AppointmentPlan {
    std::string request_id;
    m05::Remeasurement request;
    std::uint64_t expected_revision{};
    ResourceUse camera;
    std::vector<Exposure> exposures; // Absolute intervals in E09 time.
    Tick result_by{}, ready_by{}, latest_ready{};
};

struct RemeasurementReply {
    Reply status{Reply::Uncertain};
    std::string appointment_id;
};

class RemeasurementPort {
public:
    virtual ~RemeasurementPort() = default;
    // Atomic, idempotent on request_id, and bound to the camera, visibility
    // time, target and snapshot. The IF-004 adapter owns actual reservation,
    // effect reconciliation and release after acceptance.
    virtual RemeasurementReply book(const AppointmentPlan& plan) = 0;
};

struct Appointment {
    AppointmentPlan plan;
    std::string id;
};

struct AppointmentResult {
    std::optional<Appointment> appointment;
    Reason reason{Reason::None};
};

std::optional<AppointmentPlan> plan_remeasurement(const SchedulingConfig& config,
                                                  const RemeasurementBudget& budget,
                                                  const m05::Remeasurement& request,
                                                  const ExecutionSnapshot& snapshot,
                                                  std::span<const ExposureWindow> visibility,
                                                  const Deadlines& downstream,
                                                  std::string request_id, Tick now);
AppointmentResult book_remeasurement(const AppointmentPlan& plan, RemeasurementPort& port, Tick now);

struct ExposureReceipt {
    std::string component;
    Interval time;
};

struct RemeasurementObservation {
    m05::RemeasurementProof proof;
    std::vector<ExposureReceipt> exposures;
    Tick available{};
};

struct RemeasurementResult {
    std::optional<m05::Result> precheck;
    Reason reason{Reason::None};
};

RemeasurementResult recheck_remeasurement(const Appointment& appointment,
                                          const RemeasurementObservation& observation,
                                          const m05::EligibilityService& eligibility,
                                          const CurrentTarget& current,
                                          const m05::Snapshot& snapshot, Tick now);

}  // namespace waste::m06
