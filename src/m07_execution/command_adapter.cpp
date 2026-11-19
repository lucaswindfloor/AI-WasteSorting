#include "command_adapter.h"

#include <algorithm>
#include <cmath>

namespace waste::m07 {
namespace {

bool finite_ref(const m04::Reference& r) {
    return r.time >= Tick::zero() && r.time_error >= Tick::zero() && std::isfinite(r.count) &&
           std::isfinite(r.count_error) && r.count_error >= 0;
}

bool compatible_reference(const m06::Task& t) {
    const auto& original = t.candidate.target.reference;
    if (!t.candidate.contact.center || !finite_ref(original) || !finite_ref(t.reference) ||
        !std::isfinite(t.pick.x) || !std::isfinite(t.pick.y) || !std::isfinite(t.pick.z) ||
        !std::isfinite(t.applied_mm) || t.reference.encoder_epoch != original.encoder_epoch) {
        return false;
    }
    if (t.tracking == m06::TrackingMode::ControllerNative) {
        return t.reference == original && t.pick == *t.candidate.contact.center && t.applied_mm == 0;
    }
    return t.reference == t.contact.current_reference && t.pick == t.contact.current_pick &&
           t.reference.time >= original.time &&
           t.pick.x == t.candidate.contact.center->x + t.applied_mm &&
           t.pick.y == t.candidate.contact.center->y && t.pick.z == t.candidate.contact.center->z;
}

bool matches_resources(const AttemptRecord& r, const m06::Task& task, const Lane& lane) {
    if (r.reservation.uses.size() != 3) {
        return false;
    }
    const std::array expected{m06::ResourceKey{m06::ResourceKind::Robot, lane.robot},
                              m06::ResourceKey{m06::ResourceKind::Tool, task.candidate.tool.identity.tool},
                              m06::ResourceKey{m06::ResourceKind::Bin, task.candidate.bin.identity.physical_bin}};
    return std::ranges::all_of(expected, [&](const auto& resource) {
        return std::ranges::any_of(r.reservation.uses, [&](const auto& use) {
            return resource == use.resource && use.time.begin >= task.handoff_at;
        });
    });
}

}  // namespace

std::optional<m06::Window> applicable_receipt(const m06::Task& task) {
    const auto& d = task.deadlines;
    if (d.handoff_to_receipt_max < d.handoff_to_receipt_min) {
        return {};
    }
    const auto open = m06::add_ticks(task.handoff_at, d.handoff_to_receipt_min);
    const auto close = m06::add_ticks(task.handoff_at, d.handoff_to_receipt_max);
    return open && close ? m06::intersection(d.receive, {*open, *close}) : std::nullopt;
}

Payload payload_from(const m06::Task& t) {
    const auto& c = t.candidate;
    const auto& d = t.deadlines;
    return {c.target, t.reservation.token, t.schedule_version, t.forecast_version,
            c.recipe_version, c.proof_id, c.profile_version, c.region_id, c.bin_mapping_version,
            c.tool.identity, c.bin.identity, c.bin.placement_revision, t.contract, t.tracking,
            t.pick, t.reference, t.applied_mm, t.contact.instants, applicable_receipt(t).value_or(d.receive), d.handoff,
            d.early_margin, d.late_margin, d.receive_to_contact_min, d.receive_to_contact_max,
            t.handoff_at, c.valid_until, t.expected_revision,
            c.remeasurement ? std::optional(c.remeasurement->appointment_id) : std::nullopt};
}

bool same_handoff(const m06::Task& a, const m06::Task& b) {
    return a.attempt == b.attempt && a.reservation == b.reservation &&
           a.candidate.snapshot_revision == b.candidate.snapshot_revision &&
           a.candidate.contact.sources == b.candidate.contact.sources &&
           a.candidate.route_dependencies == b.candidate.route_dependencies &&
           a.candidate.tool == b.candidate.tool && a.candidate.bin == b.candidate.bin &&
           a.deadlines.receive == b.deadlines.receive &&
           a.deadlines.handoff_to_receipt_min == b.deadlines.handoff_to_receipt_min &&
           a.deadlines.handoff_to_receipt_max == b.deadlines.handoff_to_receipt_max &&
           payload_from(a) == payload_from(b);
}

Issue check_task(const ExecutionConfig& c, const DurableState& s,
                 const AttemptRecord& r, const m06::Task& t, Tick now) {
    const auto* lane = lane_for(c, t.candidate.tool.identity.station);
    if (!lane || t.reservation != r.reservation || t.attempt != r.original.attempt ||
        t.candidate.target != r.original.target || t.candidate.target.generation != c.generation ||
        t.handoff_at != r.original.handoff_at || t.contract != lane->contract || t.tracking != lane->tracking ||
        t.schedule_version.empty() || t.forecast_version.empty() || t.candidate.proof_id.empty() ||
        t.candidate.region_id.empty() || !matches_resources(r, t, *lane) ||
        !compatible_reference(t) || t.reference.time > now || !applicable_receipt(t) ||
        t.candidate.contact.check.state != m05::State::Ready ||
        t.deadlines.receipt != (t.contract == m06::Contract::A ? m06::Receipt::CommitT7 : m06::Receipt::FinalGoalT4) ||
        !m06::valid(t.contact.instants) || !m06::valid(t.deadlines.receive) || !m06::valid(t.deadlines.handoff) ||
        t.deadlines.early_margin < Tick::zero() || t.deadlines.late_margin < Tick::zero() ||
        t.deadlines.receive_to_contact_min < Tick::zero() ||
        t.deadlines.receive_to_contact_max < t.deadlines.receive_to_contact_min ||
        t.handoff_at < t.deadlines.handoff.open || t.handoff_at > t.deadlines.handoff.close ||
        t.deadlines.handoff.close > r.original.handoff_deadline ||
        t.deadlines.receive.close > t.contact.instants.close ||
        t.contact.instants.close >= t.candidate.valid_until) {
        return Issue::InvalidInput;
    }
    const auto receipt = *applicable_receipt(t);
    const auto guard_open = m06::add_ticks(t.contact.instants.open, t.deadlines.early_margin);
    const auto first = m06::add_ticks(receipt.open, t.deadlines.receive_to_contact_min);
    const auto last = m06::add_ticks(receipt.close, t.deadlines.receive_to_contact_max);
    const auto guard_last = last ? m06::add_ticks(*last, t.deadlines.late_margin) : std::nullopt;
    if (!guard_open || !first || !guard_last || *first < *guard_open ||
        *guard_last > t.contact.instants.close) {
        return Issue::InvalidInput;
    }
    if (now >= t.candidate.valid_until ||
        (r.stage == Stage::Held && (now > t.handoff_at || now >= r.reservation.valid_until))) {
        return Issue::Expired;
    }
    if (!gate_open(c, s.field ? &*s.field : nullptr, now)) {
        return Issue::FieldBlocked;
    }
    const auto* link = current_link(s.field->controllers, lane->controller, now, c.max_field_age);
    const auto* terminal = current_link(s.field->terminals, lane->terminal, now, c.max_field_age);
    if (!link || !terminal || (r.controller && r.controller != ControllerIdentity{link->device, link->session})) {
        return Issue::FieldBlocked;
    }
    if (r.stage == Stage::Held &&
        (t.expected_revision != s.revision || t.candidate.snapshot_revision != s.revision)) {
        return Issue::StaleRevision;
    }
    return Issue::None;
}

std::optional<Command> command_for(const AttemptRecord& r, Operation o) {
    if (!r.task || !r.controller) {
        return {};
    }
    const bool contract_a = r.task->contract == m06::Contract::A;
    if ((o == Operation::Prepare && !contract_a) ||
        (o == Operation::Commit && (!contract_a || !r.plan)) ||
        (o == Operation::FinalGoal && contract_a)) {
        return {};
    }
    Command command{{r.task->attempt, o}, *r.controller, payload_from(*r.task), {}, {}};
    if (o == Operation::Commit) {
        command.plan_id = r.plan->plan_id;
    } else if (o == Operation::Cancel) {
        for (auto original : {Operation::Commit, Operation::FinalGoal, Operation::Prepare}) {
            const auto found = r.intents.find(original);
            if (found != r.intents.end() && found->second.progress != Progress::NotSent) {
                command.related_request = found->second.wire.command.request;
                if (r.plan) {
                    command.plan_id = r.plan->plan_id;
                }
                return command;
            }
        }
        return {};
    }
    return command;
}

}  // namespace waste::m07
