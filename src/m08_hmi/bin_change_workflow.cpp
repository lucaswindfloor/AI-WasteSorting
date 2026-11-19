#include "bin_change_workflow.h"

namespace waste::m08 {

Transition bin_transition(const Settings& s, const Operation& op, const Runtime& r, Tick now) {
    const auto* bin = find_bin(r, op.request.object);
    if (!bin || !fresh(bin->stamp, now, s.max_fact_age)) {
        return {Phase::WaitingDevice, Issue::Conditions};
    }
    if (!op.last_applied) {
        if (bin->placement != op.old_placement || bin->physical != op.old_physical ||
            bin->mapping != op.old_mapping || !bin->fence.empty() || r.epoch != op.request.epoch) {
            return {Phase::Failed, Issue::Conflict};
        }
        return {Phase::Accepted, Issue::None, Step::FenceBin};
    }
    const bool own_fence = bin->fence == op.request.key.id;
    const bool replaced = bin->present && bin->physical == op.request.value && bin->placement > op.old_placement;
    if (r.epoch != op.request.epoch) {
        return {Phase::NeedsReview, Issue::Conflict};
    }
    if (*op.last_applied == Step::FenceBin) {
        if (!own_fence || !bin->no_unresolved || !bin->change_permitted ||
            !fresh(r.safety.stamp, now, s.max_fact_age) || !r.safety.permits) {
            return {Phase::WaitingClear, Issue::Conditions};
        }
        if (!replaced) {
            return {Phase::CanChange};
        }
        return {Phase::WaitingMapping, Issue::None, Step::MapBin};
    }
    if (*op.last_applied == Step::MapBin) {
        if (own_fence && replaced && bin->mapping > op.old_mapping && bin->no_unresolved &&
            fresh(r.safety.stamp, now, s.max_fact_age) && r.safety.permits) {
            return {Phase::WaitingMapping, Issue::None, Step::UnfenceBin};
        }
        return {Phase::WaitingMapping, Issue::Conditions};
    }
    if (*op.last_applied == Step::UnfenceBin) {
        return replaced && bin->fence.empty() && bin->mapping > op.old_mapping && bin->usable && !bin->full
               ? Transition{Phase::Applied} : Transition{Phase::WaitingMapping, Issue::Conditions};
    }
    return {Phase::NeedsReview, Issue::Invalid};
}

}  // namespace waste::m08
