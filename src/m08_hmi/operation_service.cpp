#include "operation_service.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace waste::m08 {
namespace {

bool configuration(Action a) { return a == Action::Publish || a == Action::Rollback; }

bool valid_text(std::string_view value, std::size_t max = 96) {
    return value.size() <= max && std::ranges::all_of(value, [](unsigned char byte) {
        return byte >= 32 && byte != 127;
    });
}

bool known_action(Action action) {
    return action == Action::Start || action == Action::Pause || action == Action::Drain ||
           action == Action::ChangeBin || action == Action::Approve || action == Action::Publish ||
           action == Action::Rollback || action == Action::Acknowledge;
}

void audit(Journal& j, const Settings& s, AuditEvent item) {
    if (j.audit.size() >= s.max_operations * 2) {
        j.audit.erase(j.audit.begin());
    }
    j.audit.push_back(std::move(item));
}

Transition line_transition(const Settings& s, const Operation& op, const Runtime& r, Tick now) {
    if (!op.last_applied) {
        switch (op.request.action) {
        case Action::Start: return {Phase::Accepted, Issue::None, Step::Start};
        case Action::Pause: return {Phase::Accepted, Issue::None, Step::Pause};
        case Action::Drain: return {Phase::Accepted, Issue::None, Step::Drain};
        default: return {Phase::Failed, Issue::Invalid};
        }
    }
    const auto mode = *op.last_applied == Step::Start ? LineMode::Running
                    : *op.last_applied == Step::Pause ? LineMode::Paused : LineMode::Ended;
    return r.epoch == op.request.epoch && fresh(r.line.stamp, now, s.max_fact_age) &&
           r.line.stamp.observed >= op.issued && r.line.mode == mode &&
           (mode != LineMode::Ended || r.line.drained)
           ? Transition{Phase::Applied} : Transition{Phase::WaitingDevice};
}

Transition next_step(const Settings& s, const Journal& journal, const Operation& op, const Runtime& r, Tick now) {
    if (op.request.action == Action::ChangeBin) {
        return bin_transition(s, op, r, now);
    }
    if (configuration(op.request.action)) {
        return config_transition(s, journal, op, r, now);
    }
    return line_transition(s, op, r, now);
}

bool guard(const Runtime& r, const Command& command) {
    return r.scope == command.key.operation.scope && r.revision == command.runtime_revision &&
           r.epoch == command.epoch;
}

}  // namespace

OperationService::OperationService(Settings settings, IdentityPort& identity, WorkCellPort& cell,
                                  JournalPort& store, Clock clock)
    : settings_(std::move(settings)), identity_(identity), cell_(cell), store_(store), clock_(std::move(clock)) {
    if (!clock_ || settings_.scope.station.empty() || settings_.scope.run.empty() || settings_.writer.empty() ||
        !settings_.max_operations || settings_.max_operations > 4096 || !settings_.max_page ||
        !settings_.max_export_rows || settings_.max_fact_age <= Tick::zero() ||
        settings_.command_timeout <= Tick::zero() || settings_.catalog.empty() ||
        settings_.catalog.size() > 128 || !settings_.catalog.contains(settings_.approved_baseline)) {
        throw std::invalid_argument("invalid M08 configuration");
    }
    for (const auto& [id, pkg] : settings_.catalog) {
        if (id.empty() || id != pkg.id || pkg.digest.empty() || pkg.versions.empty() ||
            std::ranges::any_of(pkg.versions, [](const auto& version) {
                return version.first.empty() || version.second.empty();
            })) {
            throw std::invalid_argument("invalid package fingerprint or component versions");
        }
    }
    journal_.scope = settings_.scope;
}

Tick OperationService::time() {
    const auto next = clock_();
    if (next < now_) {
        ready_ = false;
        throw std::runtime_error("M08 clock moved backwards");
    }
    return now_ = next;
}

std::optional<Runtime> OperationService::live() {
    try {
        auto state = cell_.read(settings_.scope);
        if (state && state->scope == settings_.scope && state->revision && state->epoch) {
            return state;
        }
    } catch (...) {
    }
    return {};
}

bool OperationService::commit(Journal next) {
    if (journal_.revision == std::numeric_limits<std::uint64_t>::max() ||
        serial_ == std::numeric_limits<std::uint64_t>::max()) {
        ready_ = false;
        return false;
    }
    next.revision = journal_.revision + 1;
    next.transaction = settings_.writer + "/" + std::to_string(++serial_);
    try {
        const auto ack = store_.commit(journal_.revision, next);
        if (ack.confirmed && ack.revision == next.revision && ack.transaction == next.transaction) {
            journal_ = std::move(next);
            return true;
        }
    } catch (...) {
    }
    ready_ = false;
    return false;
}

bool OperationService::recover() {
    ready_ = false;
    Load loaded;
    try {
        loaded = store_.load(settings_.scope);
    } catch (...) {
        return false;
    }
    if (loaded.status == LoadStatus::Restored && loaded.journal &&
        loaded.journal->scope == settings_.scope && loaded.journal->revision &&
        loaded.journal->revision >= journal_.revision &&
        !loaded.journal->transaction.empty() &&
        loaded.journal->operations.size() <= settings_.max_operations &&
        loaded.journal->activated.size() <= settings_.max_operations + 1 &&
        loaded.journal->audit.size() <= 2 * settings_.max_operations) {
        journal_ = std::move(*loaded.journal);
        ready_ = true;
        return true;
    }
    if (loaded.status != LoadStatus::Empty || loaded.journal || journal_.revision) {
        return false;
    }
    const auto r = live();
    const auto now = time();
    if (!r || !active_deployment(settings_, *r, now) ||
        r->active->package != settings_.approved_baseline || !r->line.fence.empty()) {
        return false;
    }
    auto seeded = journal_;
    seeded.approvals.emplace(r->active->package, r->active->digest);
    seeded.activated.emplace(r->epoch, *r->active);
    seeded.highest_epoch = r->epoch;
    ready_ = commit(std::move(seeded));
    return ready_;
}

OperationReply OperationService::fail(const Request& request, std::string actor, Issue issue) {
    if (ready_ && request.key.scope == settings_.scope && valid_text(request.key.id) && known_action(request.action)) {
        auto next = journal_;
        auto basis = request;
        if (!valid_text(basis.object)) basis.object.clear();
        if (!valid_text(basis.value)) basis.value.clear();
        audit(next, settings_, {std::move(basis), std::move(actor), issue, time()});
        if (!commit(std::move(next))) {
            return {Issue::Persistence, {}};
        }
    }
    return {issue, {}};
}

OperationReply OperationService::submit(std::string_view credential, const Request& request) {
    const auto principal = identity_.session(credential);
    if (!principal || !allows(*principal, settings_.scope, request.action) || request.key.scope != settings_.scope) {
        return fail(request, principal ? principal->subject : "", Issue::Unauthorized);
    }
    if (!ready_) {
        return {Issue::NotReady, {}};
    }
    if (!known_action(request.action) || request.key.id.empty() || !valid_text(request.key.id) ||
        !valid_text(request.object) || !valid_text(request.value)) {
        return fail(request, principal->subject, Issue::Invalid);
    }
    if (const auto found = journal_.operations.find(request.key); found != journal_.operations.end()) {
        if (found->second.actor != principal->subject || found->second.request != request) {
            return fail(request, principal->subject, Issue::Conflict);
        }
        return {Issue::None, found->second};
    }
    if (journal_.operations.size() >= settings_.max_operations) {
        return fail(request, principal->subject, Issue::Capacity);
    }
    const auto r = live();
    const auto now = time();
    if (!r) {
        return fail(request, principal->subject, Issue::NotReady);
    }
    if (const auto issue = preflight(settings_, journal_, *r, request, now); issue != Issue::None) {
        return fail(request, principal->subject, issue);
    }
    auto next = journal_;
    Operation op;
    op.actor = principal->subject;
    op.request = request;
    op.created = op.updated = now;
    if (request.action == Action::Approve) {
        next.approvals[request.object] = settings_.catalog.at(request.object).digest;
        op.phase = Phase::Applied;
    } else if (request.action == Action::Acknowledge) {
        next.acknowledgements[request.object] = principal->subject;
        op.phase = Phase::Applied;
    } else if (request.action == Action::ChangeBin) {
        const auto* bin = find_bin(*r, request.object);
        op.old_physical = bin->physical;
        op.old_mapping = bin->mapping;
        op.old_placement = bin->placement;
    } else if (configuration(request.action)) {
        const auto newest = std::max(next.highest_epoch, r->epoch);
        if (newest == std::numeric_limits<std::uint64_t>::max()) {
            return fail(request, principal->subject, Issue::Capacity);
        }
        op.deployment_epoch = next.highest_epoch = newest + 1;
        op.prior_fence = r->line.fence;
    }
    audit(next, settings_, {request, op.actor, Issue::None, now});
    next.operations.emplace(request.key, op);
    return commit(std::move(next)) ? OperationReply{Issue::None, std::move(op)}
                                  : OperationReply{Issue::Persistence, {}};
}

OperationReply OperationService::query(std::string_view credential, const OperationKey& key) {
    const auto actor = identity_.session(credential);
    if (!actor || key.scope != settings_.scope ||
        (!allows(*actor, key.scope, Permission::View) && !allows(*actor, key.scope, Permission::Audit))) {
        return {Issue::Unauthorized, {}};
    }
    if (!ready_) {
        return {Issue::NotReady, {}};
    }
    const auto found = journal_.operations.find(key);
    if (found == journal_.operations.end() || !can_read(*actor, found->second)) {
        return {Issue::NotFound, {}};
    }
    return {Issue::None, found->second};
}

std::optional<Workbench> OperationService::workbench(std::string_view credential) {
    const auto actor = identity_.session(credential);
    if (!actor || !allows(*actor, settings_.scope, Permission::View)) {
        return {};
    }
    const auto r = live();
    return r ? std::optional(project(settings_, journal_, *r, *actor, ready_, time())) : std::nullopt;
}

bool OperationService::authorized_to_send(const Operation& op) {
    const auto principal = identity_.current(op.actor);
    return principal && principal->subject == op.actor &&
           allows(*principal, settings_.scope, op.request.action);
}

bool OperationService::set_phase(const OperationKey& key, Phase phase, Issue issue, Tick now) {
    const auto& original = journal_.operations.at(key);
    if (original.phase == phase && original.issue == issue) {
        return true;
    }
    auto next = journal_;
    auto& op = next.operations.at(key);
    op.phase = phase;
    op.issue = issue;
    op.updated = now;
    return commit(std::move(next));
}

void OperationService::dispatch(const OperationKey& key, Step step, const Runtime& r, Tick now) {
    const auto& op = journal_.operations.at(key);
    if (!authorized_to_send(op)) {
        set_phase(key, op.last_applied ? Phase::NeedsReview : Phase::Failed, Issue::Unauthorized, now);
        return;
    }
    if (!op.last_applied) {
        auto first = op.request;
        first.revision = r.revision;
        first.epoch = op.request.epoch;
        // Exclude this admitted operation from the competing-operation check.
        auto other = journal_;
        other.operations.erase(key);
        const auto valid = preflight(settings_, other, r, first, now);
        if (valid != Issue::None) {
            set_phase(key, Phase::Failed, valid, now);
            return;
        }
    }
    Command command{{key, step}, op.request.object, op.request.value, r.line.fence,
                    r.revision, r.epoch, 0, 0, {}, op.deployment_epoch};
    if (const auto* bin = find_bin(r, op.request.object); op.request.action == Action::ChangeBin && bin) {
        command.fence = bin->fence;
        command.placement = bin->placement;
        command.mapping = bin->mapping;
    }
    if (configuration(op.request.action)) {
        command.package = settings_.catalog.at(op.request.object);
    }
    const auto preceding_issue_time = op.issued;
    auto next = journal_;
    auto& saved = next.operations.at(key);
    saved.pending = command;
    saved.issued = saved.updated = now;
    saved.phase = Phase::WaitingDevice;
    saved.issue = Issue::None;
    if (!commit(std::move(next))) {
        return;
    }
    const auto newest = live();
    auto pending = journal_.operations.at(key);
    // The command being armed must be checked against the previous step’s evidence cutoff.
    pending.issued = preceding_issue_time;
    auto final_issue = Issue::None;
    const auto checked_at = time();
    if (!newest || !guard(*newest, command)) {
        final_issue = Issue::Conflict;
    } else if (!authorized_to_send(pending)) {
        final_issue = Issue::Unauthorized;
    } else if (next_step(settings_, journal_, pending, *newest, checked_at).next != step) {
        final_issue = Issue::Conditions;
    } else if (!pending.last_applied) {
        auto other = journal_;
        other.operations.erase(key);
        auto request = pending.request;
        request.revision = newest->revision;
        final_issue = preflight(settings_, other, *newest, request, checked_at);
    }
    if (final_issue != Issue::None) {
        auto unused = journal_;
        auto& previous = unused.operations.at(key);
        previous.pending.reset(); // This process can prove it never called submit.
        previous.phase = previous.last_applied ? Phase::NeedsReview : Phase::Failed;
        previous.issue = final_issue;
        previous.updated = time();
        commit(std::move(unused));
        return;
    }
    Delivery delivery = Delivery::Unknown;
    try {
        delivery = cell_.submit(command);
    } catch (...) {
    }
    if (delivery == Delivery::Unknown) {
        set_phase(key, Phase::NeedsReview, Issue::EffectUnknown, time());
    } else if (delivery == Delivery::Rejected) {
        auto rejected = journal_;
        auto& entry = rejected.operations.at(key);
        entry.pending.reset();
        entry.phase = Phase::Failed;
        entry.issue = Issue::DeviceRejected;
        entry.updated = time();
        commit(std::move(rejected));
    }
}

void OperationService::advance_one(const OperationKey& key, Tick now) {
    const auto& op = journal_.operations.at(key);
    if (op.pending) {
        Effect effect;
        try {
            effect = cell_.lookup(op.pending->key);
        } catch (...) {
        }
        now = time();
        if (effect.key != op.pending->key || !fresh(effect.stamp, now, settings_.max_fact_age) ||
            effect.stamp.observed < op.issued || !valid_text(effect.evidence, 256) || effect.evidence.empty() ||
            (effect.state != EffectState::Applied && effect.state != EffectState::Rejected)) {
            if (now - op.issued >= settings_.command_timeout) {
                set_phase(key, Phase::NeedsReview, Issue::EffectUnknown, now);
            }
            return;
        }
        auto next = journal_;
        auto& row = next.operations.at(key);
        if (effect.state == EffectState::Applied) {
            row.last_applied = row.pending->key.step;
            row.phase = Phase::WaitingDevice;
            row.issue = Issue::None;
        } else {
            row.phase = Phase::Failed;
            row.issue = row.pending->key.step == Step::StageConfig ? Issue::LoadFailed : Issue::DeviceRejected;
        }
        row.evidence.push_back(std::move(effect.evidence));
        row.pending.reset();
        row.updated = now;
        commit(std::move(next));
        return;
    }
    if (op.phase == Phase::NeedsReview) {
        return;
    }
    const auto r = live();
    if (!r) {
        return;
    }
    now = time();
    const auto move = next_step(settings_, journal_, op, *r, now);
    if (move.record_activation) {
        auto next = journal_;
        next.activated.emplace(op.deployment_epoch, *r->active);
        next.operations.at(key).updated = now;
        commit(std::move(next));
    } else if (move.next) {
        dispatch(key, *move.next, *r, now);
    } else {
        set_phase(key, move.phase, move.issue, now);
    }
}

void OperationService::advance() {
    const auto now = time();
    if (!ready_) {
        return;
    }
    std::vector<OperationKey> pending;
    for (const auto& [key, op] : journal_.operations) {
        if (!terminal(op.phase)) {
            pending.push_back(key);
        }
    }
    for (const auto& key : pending) {
        if (!ready_) {
            break;
        }
        advance_one(key, now);
    }
}

}  // namespace waste::m08
