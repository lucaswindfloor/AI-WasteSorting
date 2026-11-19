#include "fixtures.h"

namespace waste::m05::test {

void remeasurement_test() {
    auto r = recipe();
    r.materials.at("PET").allow_remeasurement = true;
    r.remeasurement_components = {MissingComponent::Coverage, MissingComponent::CenterHeight, MissingComponent::Normal};
    const EligibilityService service(r);
    auto t = target();
    const auto snapshot = facts();
    auto g = grid(t);
    g.cells[12] = {CellState::Missing};
    const auto pending = evaluate(service, t, snapshot, g);
    check(pending.state == State::NeedsRemeasurement && pending.candidates.empty() &&
          pending.remeasurements.size() == 1 && pending.remeasurements[0].required.size() == 2,
          "approved missing coverage did not create a separate remeasurement request");
    g.cells[13] = {CellState::Neighbor};
    check(evaluate(service, t, snapshot, g).remeasurements.empty(), "known neighbor sent to remeasurement");
    const auto& request = pending.remeasurements.front();
    ++t.version;
    auto fresh = grid(t, "E11-verified-coverage");
    RemeasurementProof proof{request.target, binding(t), "A", fresh.proof_id, "appointment-2",
                             22ms, 35ms, true};
    const std::array area{Region{"A", 0, 406, 0}};
    const Observation new_surface{std::span(&fresh, 1), area};
    auto improved = service.evaluate_after_remeasurement(request, proof, t, snapshot, new_surface, 23ms);
    check(improved.state == State::Ready && improved.candidates[0].target.version == t.version &&
          improved.candidates[0].valid_until == 35ms && improved.candidates[0].remeasurement &&
          improved.candidates[0].remeasurement->appointment_id == "appointment-2", "same target remeasurement was not fully rechecked");
    auto wrong = proof;
    wrong.observed.id = 99;
    check(service.evaluate_after_remeasurement(request, wrong, t, snapshot, new_surface, 23ms).reason ==
              Reason::RemeasurementRejected, "wrong item remeasurement accepted");
    check(service.evaluate_after_remeasurement(request, proof, t, snapshot, new_surface, 35ms).reason ==
              Reason::RemeasurementRejected, "expired remeasurement accepted");
    auto unavailable = snapshot;
    unavailable.revision++;
    unavailable.bins[0].condition = BinCondition::Full;
    unavailable.bins[1].condition = BinCondition::Unavailable;
    check(service.evaluate_after_remeasurement(request, proof, t, unavailable, new_surface, 23ms).candidates.empty(),
          "remeasurement skipped rechecking the latest bin");
}

}  // namespace waste::m05::test
