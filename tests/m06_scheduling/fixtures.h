#pragma once

#include "../m05_eligibility/fixtures.h"
#include "m06_scheduling/scheduling_service.h"

namespace waste::m06::test {

using namespace std::chrono_literals;
using m02::test::check;
using m02::test::near;

inline m05::Recipe recipe(bool e11 = false) {
    auto r = m05::test::recipe();
    r.valid_until = 500ms;
    r.max_fact_age = 400ms;
    if (e11) {
        r.materials.at("PET").allow_remeasurement = true;
        r.remeasurement_components = {m05::MissingComponent::Coverage, m05::MissingComponent::CenterHeight};
    }
    return r;
}

inline SchedulingConfig config(Contract contract = Contract::B) {
    SchedulingConfig c;
    c.generation = {"P0", 1};
    c.version = "schedule-1";
    c.scheduler_session = "scheduler-test-1";
    c.valid_until = 500ms;
    c.max_prediction_age = 100ms;
    c.transaction_lead = 3ms;
    c.max_tracking_error_mm = 15;
    c.max_batch = 32;
    c.max_attempts = 32;
    c.max_resources = 32;
    c.max_intervals = 128;
    c.max_retries = 2;
    const auto b = contract == Contract::A ? Budget{15ms, 20ms, 58ms, 60ms, 2ms, 5ms}
                                          : Budget{5ms, 10ms, 85ms, 90ms, 2ms, 5ms};
    c.lanes.push_back({"arm-A", "R1", contract, TrackingMode::ControllerNative,
                       {{"pick-belt", {270, 400, 40}, {330, 420, 56}}}, b,
                       {0ms, 145ms}, {0ms, 135ms}, {112ms, 125ms}});
    return c;
}

inline ExecutionSnapshot snapshot() {
    auto eligibility = m05::test::facts();
    eligibility.tools[0].time.valid_until = 500ms;
    for (auto& bin : eligibility.bins) {
        bin.time.valid_until = 500ms;
    }
    const auto calendar = [](ResourceKind kind, std::string id) {
        return ResourceCalendar{{kind, std::move(id)}, 440ms, {{0ms, 500ms}}, {}};
    };
    return {eligibility, {calendar(ResourceKind::Robot, "R1"),
                          calendar(ResourceKind::Tool, "cup-1"),
                          calendar(ResourceKind::Bin, "PET-bin-1"),
                          calendar(ResourceKind::Bin, "PET-bin-2"),
                          calendar(ResourceKind::E11, "camera-11")}, {}, {}};
}

inline CurrentTarget current(m04::TargetId id = 42, bool missing = false) {
    auto t = m05::test::target();
    t.id = id;
    t.valid_until = 490ms;
    t.material.valid_until = 480ms;
    for (auto& patch : t.surfaces) {
        patch.valid_until = 480ms;
    }
    auto g = m05::test::grid(t);
    g.valid_until = 480ms;
    if (missing) {
        g.cells[12] = {m05::CellState::Missing};
    }
    auto r = t.footprint.reference;
    r.time = 50ms;
    r.time_error = 0ns;
    r.count += 1600;
    m04::Prediction p{t.id, t.version, t.generation, m04::Reason::None,
                      t.footprint.reference, r, {1000, 999, 999}, 80, 0, 450ms};
    MotionForecast f{t.generation, r, 2000, 2000, 450ms, "constant-bench-speed"};
    return {std::move(t), p, f, {std::move(g)}, {{"A", 0, 406, 0}}};
}

inline QueuedCandidate queued(const m05::EligibilityService& m05, const ExecutionSnapshot& m07,
                                const CurrentTarget& input, int priority = 0, std::uint64_t order = 0) {
    const auto precheck = m05.evaluate(input.target, m07.eligibility, {input.grids, input.regions}, 50ms);
    check(precheck.candidates.size() == 1, "fixture failed M05 precheck");
    return {precheck.candidates.front(), priority, order};
}

void contact_test();
void timing_test();
void resource_test();
void remeasurement_test();

}  // namespace waste::m06::test
