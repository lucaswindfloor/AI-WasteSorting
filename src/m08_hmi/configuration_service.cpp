#include "configuration_service.h"

#include <algorithm>

namespace waste::m08 {

bool describes_deployment(const Deployment& f, const Package& pkg, const Operation& op,
                           Tick now, Tick max_age) {
    return fresh(f.stamp, now, max_age) && f.stamp.observed >= op.issued &&
           f.operation == op.request.key.id && f.epoch == op.deployment_epoch &&
           f.package == pkg.id && f.digest == pkg.digest && f.actual == pkg.versions && f.problems.empty();
}

Transition config_transition(const Settings& s, const Journal& journal, const Operation& op,
                             const Runtime& r, Tick now) {
    const auto pkg = s.catalog.find(op.request.object);
    if (pkg == s.catalog.end()) {
        return {Phase::NeedsReview, Issue::Invalid};
    }
    if (!fresh(r.line.stamp, now, s.max_fact_age)) {
        return {Phase::WaitingDevice, Issue::Conditions};
    }
    if (!op.last_applied) {
        if (r.epoch != op.request.epoch || r.line.fence != op.prior_fence) {
            return {Phase::NeedsReview, Issue::Conflict};
        }
        return {Phase::Accepted, Issue::None, Step::FenceLine};
    }
    const bool own_fence = r.line.fence == op.request.key.id;
    if (*op.last_applied == Step::FenceLine) {
        return own_fence && r.line.drained && r.line.mode != LineMode::Running
               ? Transition{Phase::WaitingActivation, Issue::None, Step::StageConfig}
               : Transition{Phase::WaitingClear, Issue::Conditions};
    }
    if (*op.last_applied == Step::StageConfig) {
        if (own_fence && r.line.drained && r.line.mode != LineMode::Running && r.staged &&
            describes_deployment(*r.staged, pkg->second, op, now, s.max_fact_age)) {
            return {Phase::WaitingActivation, Issue::None, Step::ActivateConfig};
        }
        if (own_fence && r.staged && r.staged->operation == op.request.key.id &&
            r.staged->epoch == op.deployment_epoch && r.staged->stamp.observed >= op.issued &&
            fresh(r.staged->stamp, now, s.max_fact_age) &&
            (!r.staged->problems.empty() || r.staged->digest != pkg->second.digest || r.staged->package != pkg->second.id ||
             std::ranges::any_of(r.staged->actual, [&](const auto& loaded) {
                 const auto expected = pkg->second.versions.find(loaded.first);
                 return expected == pkg->second.versions.end() || expected->second != loaded.second;
             }))) {
            return {Phase::Failed, Issue::LoadFailed};
        }
        return {Phase::WaitingActivation, Issue::Conditions};
    }
    const bool active = r.active && r.epoch == op.deployment_epoch &&
                        describes_deployment(*r.active, pkg->second, op, now, s.max_fact_age);
    if (*op.last_applied == Step::ActivateConfig) {
        if (!own_fence || !active || !r.line.drained) {
            return {Phase::WaitingActivation, Issue::Conditions};
        }
        if (!journal.activated.contains(op.deployment_epoch)) {
            return {Phase::WaitingActivation, Issue::None, {}, true};
        }
        return {Phase::WaitingActivation, Issue::None, Step::UnfenceLine};
    }
    if (*op.last_applied == Step::UnfenceLine) {
        const auto confirmed = journal.activated.find(op.deployment_epoch);
        if (confirmed != journal.activated.end() && r.active && r.epoch == op.deployment_epoch &&
            r.line.fence.empty()) {
            auto activation = op;
            activation.issued = confirmed->second.stamp.observed;
            if (describes_deployment(*r.active, pkg->second, activation, now, s.max_fact_age)) {
                return {Phase::Applied};
            }
        }
        return {Phase::WaitingActivation, Issue::Conditions};
    }
    return {Phase::NeedsReview, Issue::Invalid};
}

}  // namespace waste::m08
