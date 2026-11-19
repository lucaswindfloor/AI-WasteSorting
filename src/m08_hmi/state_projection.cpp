#include "state_projection.h"

#include <algorithm>

namespace waste::m08 {

bool fresh(const Stamp& s, Tick now, Tick age) {
    return s.valid && !s.source.empty() && s.observed >= Tick::zero() && s.observed <= now &&
           now < s.valid_until && now - s.observed < age;
}

bool terminal(Phase p) { return p == Phase::Applied || p == Phase::Failed; }

const Bin* find_bin(const Runtime& runtime, std::string_view slot) {
    const auto it = std::ranges::find(runtime.bins, slot, &Bin::slot);
    return it == runtime.bins.end() ? nullptr : &*it;
}

bool active_deployment(const Settings& s, const Runtime& r, Tick now) {
    if (!r.active || r.active->epoch != r.epoch || !fresh(r.active->stamp, now, s.max_fact_age) || !r.active->problems.empty()) {
        return false;
    }
    const auto package = s.catalog.find(r.active->package);
    return package != s.catalog.end() && package->second.digest == r.active->digest &&
           package->second.versions == r.active->actual;
}

Issue preflight(const Settings& settings, const Journal& journal, const Runtime& r,
                const Request& request, Tick now) {
    if (request.key.scope != settings.scope || r.scope != settings.scope ||
        !r.revision || request.revision != r.revision || request.epoch != r.epoch) {
        return Issue::StaleView;
    }
    if (request.action == Action::Acknowledge) {
        const auto alarm = std::ranges::find(r.alarms, request.object, &Alarm::id);
        return alarm != r.alarms.end() && alarm->active ? Issue::None : Issue::NotFound;
    }
    if (request.action == Action::Approve) {
        return settings.catalog.contains(request.object) ? Issue::None : Issue::Invalid;
    }
    if (!fresh(r.line.stamp, now, settings.max_fact_age)) {
        return Issue::Conditions;
    }
    for (const auto& [key, work] : journal.operations) {
        if (terminal(work.phase)) {
            continue;
        }
        const auto other = work.request.action;
        const bool other_config = other == Action::Publish || other == Action::Rollback;
        const bool config = request.action == Action::Publish || request.action == Action::Rollback;
        const bool other_line = other == Action::Start || other == Action::Pause || other == Action::Drain;
        const bool line = request.action == Action::Start || request.action == Action::Pause ||
                          request.action == Action::Drain;
        if ((other_config && (config || request.action == Action::Start || request.action == Action::ChangeBin)) ||
            (config && (other_line || other == Action::ChangeBin)) ||
            (line && other_line) ||
            (request.action == Action::ChangeBin && other == Action::ChangeBin &&
             work.request.object == request.object)) {
            return Issue::Conflict;
        }
    }
    switch (request.action) {
    case Action::Start: {
        const auto activated = journal.activated.find(r.epoch);
        return r.line.fence.empty() && r.line.ready &&
               std::ranges::all_of(r.bins, [&](const Bin& bin) {
                   return fresh(bin.stamp, now, settings.max_fact_age);
               }) &&
               std::ranges::any_of(r.bins, [](const Bin& bin) {
                   return bin.present && bin.usable && !bin.full && bin.fence.empty();
               }) &&
               (r.line.mode == LineMode::Idle || r.line.mode == LineMode::Paused || r.line.mode == LineMode::Ended) &&
               fresh(r.safety.stamp, now, settings.max_fact_age) && r.safety.permits &&
               active_deployment(settings, r, now) && r.active->package == request.object &&
               activated != journal.activated.end() && activated->second.package == r.active->package &&
               activated->second.digest == r.active->digest ? Issue::None : Issue::Conditions;
    }
    case Action::Pause: return r.line.mode == LineMode::Running ? Issue::None : Issue::Conditions;
    case Action::Drain:
        return (r.line.mode == LineMode::Running || r.line.mode == LineMode::Paused)
               ? Issue::None : Issue::Conditions;
    case Action::ChangeBin: {
        const auto* bin = find_bin(r, request.object);
        if (!bin || !bin->present || bin->physical.empty() || !bin->fence.empty() ||
            !fresh(bin->stamp, now, settings.max_fact_age) || !fresh(r.safety.stamp, now, settings.max_fact_age) ||
            !r.safety.permits || !r.line.fence.empty()) {
            return Issue::Conditions;
        }
        return !request.value.empty() && request.value != bin->physical ? Issue::None : Issue::Invalid;
    }
    case Action::Publish:
    case Action::Rollback: {
        const auto pkg = settings.catalog.find(request.object);
        const auto approved = journal.approvals.find(request.object);
        if (pkg == settings.catalog.end() || approved == journal.approvals.end() ||
            approved->second != pkg->second.digest) {
            return Issue::Conditions;
        }
        if (!r.line.fence.empty()) {
            const auto previous = journal.operations.find({r.scope, r.line.fence});
            if (request.action != Action::Rollback || previous == journal.operations.end() ||
                previous->second.phase != Phase::Failed ||
                (previous->second.request.action != Action::Publish &&
                 previous->second.request.action != Action::Rollback)) {
                return Issue::Conditions;
            }
        }
        if (request.action == Action::Rollback &&
            std::ranges::none_of(journal.activated, [&](const auto& p) {
                return p.second.package == pkg->second.id && p.second.digest == pkg->second.digest;
            })) {
            return Issue::Conditions;
        }
        return Issue::None;
    }
    case Action::Approve:
    case Action::Acknowledge: break;
    }
    return Issue::Invalid;
}

Workbench project(const Settings& settings, const Journal& journal, const Runtime& r,
                  const Principal& principal, bool operational, Tick now) {
    Workbench out;
    out.runtime = r;
    out.operational = operational;
    out.line_fresh = fresh(r.line.stamp, now, settings.max_fact_age);
    out.safety_fresh = fresh(r.safety.stamp, now, settings.max_fact_age);
    out.production_fenced = !r.line.fence.empty();
    for (const auto& bin : r.bins) {
        if (!fresh(bin.stamp, now, settings.max_fact_age)) {
            out.stale_bins.push_back(bin.slot);
        }
    }
    const auto add = [&](Action action, const std::string& object, std::string proposed = {}) {
        Issue issue = Issue::Unauthorized;
        if (allows(principal, r.scope, action)) {
            issue = operational ? preflight(settings, journal, r,
                {{r.scope, "view"}, action, object, std::move(proposed), r.revision, r.epoch}, now)
                : Issue::NotReady;
        }
        out.actions.push_back({action, object, issue});
    };
    add(Action::Start, r.active ? r.active->package : "");
    add(Action::Pause, {});
    add(Action::Drain, {});
    for (const auto& bin : r.bins) {
        // The UI obtains a new physical identity before actual submission.
        auto placeholder = bin.physical + "-prospective";
        add(Action::ChangeBin, bin.slot, std::move(placeholder));
    }
    for (const auto& alarm : r.alarms) {
        add(Action::Acknowledge, alarm.id);
    }
    for (const auto& [id, package] : settings.catalog) {
        out.packages.push_back(package);
        for (auto action : {Action::Approve, Action::Publish, Action::Rollback}) {
            add(action, id);
        }
    }
    out.approvals = journal.approvals;
    out.acknowledgements = journal.acknowledgements;
    if (allows(principal, r.scope, Permission::Audit)) {
        constexpr std::size_t audit_view_limit = 50;
        const auto count = static_cast<std::ptrdiff_t>(std::min(audit_view_limit, journal.audit.size()));
        out.audits.assign(journal.audit.end() - count, journal.audit.end());
    }
    for (const auto& [key, operation] : journal.operations) {
        if (can_read(principal, operation)) {
            out.operations.push_back(operation);
        }
    }
    return out;
}

}  // namespace waste::m08
