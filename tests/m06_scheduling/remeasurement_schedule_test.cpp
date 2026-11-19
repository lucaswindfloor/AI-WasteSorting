#include "fixtures.h"
#include "m06_scheduling/remeasurement_schedule.h"

namespace waste::m06::test {
namespace {

class BenchCamera final : public RemeasurementPort {
public:
    Reply response{Reply::Accepted};
    RemeasurementReply book(const AppointmentPlan&) override {
        return {response, response == Reply::Accepted ? "appointment-1" : ""};
    }
};

}  // namespace

void remeasurement_test() {
    auto c = config();
    c.enable_e11 = true;
    const m05::EligibilityService eligibility(recipe(true));
    const auto data = snapshot();
    const auto input = current(42, true);
    const auto awaiting = eligibility.evaluate(input.target, data.eligibility,
                                                {input.grids, input.regions}, 50ms);
    check(awaiting.remeasurements.size() == 1, "fixture lacks E11 request");
    const auto& request = awaiting.remeasurements.front();
    const auto downstream = contract_deadlines(Contract::B, c.lanes[0].budget, {145ms, 175ms});
    check(downstream && downstream->handoff.close == 70ms, "E11 fixture deadline");
    const RemeasurementBudget b{"camera-11",
                                 {{"screen", {0ms, 1ms}, {m05::MissingComponent::Coverage}},
                                  {"height", {5ms, 7ms}, {m05::MissingComponent::CenterHeight}}},
                                 10ms, 3ms, 12ms};
    auto visibility = std::array{ExposureWindow{"screen", {{52ms, 54ms}}},
                                 ExposureWindow{"height", {{57ms, 60ms}}}};
    const auto plan = plan_remeasurement(c, b, request, data, visibility, *downstream, "E11-1", 50ms);
    check(plan && plan->exposures[0].offset.begin == 52ms &&
          plan->exposures[1].offset.end == 59ms &&
          plan->ready_by == 65ms && plan->camera.time.end == 64ms,
          "all exposures or processing time not budgeted");
    auto missing_window = visibility;
    missing_window[1].visibility = {{70ms, 73ms}};
    check(!plan_remeasurement(c, b, request, data, missing_window, *downstream, "E11-2", 50ms),
          "plan accepted without time for required exposures");
    auto slow = b;
    slow.first_to_result_upper = slow.camera_busy_upper = 50ms;
    check(!plan_remeasurement(c, slow, request, data, visibility, *downstream, "E11-3", 50ms),
          "measurement-only budget accepted without downstream time");
    BenchCamera port;
    auto booked = book_remeasurement(*plan, port, 51ms);
    check(booked.appointment && booked.appointment->id == "appointment-1", "camera booking not bound");
    port.response = Reply::Uncertain;
    check(book_remeasurement(*plan, port, 51ms).reason == Reason::EffectUncertain,
          "uncertain E11 booking treated as definitely absent");

    auto updated = current();
    updated.target.version++;
    updated.grids[0].target = m05::binding(updated.target);
    updated.grids[0].proof_id = "new-calibrated-grid";
    RemeasurementObservation observed{{request.target, m05::binding(updated.target), "A",
                                      updated.grids[0].proof_id, booked.appointment->id, 58ms, 125ms, true},
                                      {{"screen", {52ms, 53ms}}, {"height", {57ms, 59ms}}}, 61ms};
    const auto accepted = recheck_remeasurement(*booked.appointment, observed, eligibility,
                                               updated, data.eligibility, 64ms);
    check(accepted.precheck && accepted.precheck->candidates.size() == 1 &&
          accepted.precheck->candidates[0].remeasurement->appointment_id == "appointment-1",
          "valid remeasurement not rechecked by M05");
    observed.exposures.pop_back();
    check(recheck_remeasurement(*booked.appointment, observed, eligibility, updated, data.eligibility, 64ms)
              .reason == Reason::RemeasurementMismatch, "missing exposure accepted");
    observed.exposures.push_back({"height", {57ms, 59ms}});
    observed.proof.same_target_verified = false;
    check(recheck_remeasurement(*booked.appointment, observed, eligibility, updated, data.eligibility, 64ms)
              .reason == Reason::RemeasurementMismatch, "wrong target accepted");
    observed.proof.same_target_verified = true;
    check(recheck_remeasurement(*booked.appointment, observed, eligibility, updated, data.eligibility, 71ms)
              .reason == Reason::RemeasurementExpired, "late result accepted");
}

}  // namespace waste::m06::test
