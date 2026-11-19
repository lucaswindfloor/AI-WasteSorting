#pragma once

#include "m08_hmi/hmi_service.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace waste::m08::test {

using namespace std::chrono_literals;
inline const Scope scope{"SORT-01", "RUN-042"};
inline constexpr std::string_view baseline = "P0-PET-v1";
inline constexpr std::string_view experimental = "E11-PET-v2";
inline constexpr std::string_view broken = "P0-HSI-invalid";
inline constexpr std::string_view manifest = "RUN-042-v1";

inline Settings settings() {
    Settings out{scope, "fixture", {}, std::string(baseline), 128, 50, 100, 20s, 4s};
    for (const Package& pkg : {
        Package{std::string(baseline), "demo/pet/1", {{"M01","rgb-hsi-v1"}, {"M02","cal-v1"}, {"M03","pet-v1"},
                 {"M04","track-v1"}, {"M05","rules-v1"}, {"M06","schedule-v1"}, {"M07","control-v1"}}, false},
        Package{std::string(experimental), "demo/e11/2", {{"M01","rgb-hsi-e11"}, {"M02","cal-v1"}, {"M03","pet-v2"},
                 {"M04","track-v1"}, {"M05","rules-v1"}, {"M06","schedule-v1"}, {"M07","control-v1"}}, true},
        Package{std::string(broken), "demo/hsi/incompatible", {{"M01","rgb-hsi-v1"}, {"M02","cal-v1"},
                 {"M03","wrong-band-order"}, {"M04","track-v1"}, {"M05","rules-v1"}, {"M06","schedule-v1"},
                 {"M07","control-v1"}}, false}
    }) {
        out.catalog.emplace(pkg.id, pkg);
    }
    return out;
}

class Identity final : public IdentityPort {
public:
    std::map<std::string, Principal, std::less<>> users;
    Identity() {
        add("operator", {Permission::View, Permission::Operate, Permission::ChangeBin, Permission::Evidence});
        add("engineer", {Permission::View, Permission::Publish, Permission::Audit, Permission::Evidence});
        add("reviewer", {Permission::View, Permission::Approve, Permission::Audit});
        add("auditor", {Permission::View, Permission::Audit, Permission::Evidence, Permission::Export});
        add("viewer", {Permission::View});
    }

    std::optional<Principal> session(std::string_view token) override { return current(token); }
    std::optional<Principal> current(std::string_view actor) override {
        const auto found = users.find(actor);
        return found == users.end() ? std::nullopt : std::optional(found->second);
    }

private:
    void add(std::string id, std::initializer_list<Permission> rights) {
        Principal principal{id, {}};
        for (auto right : rights) {
            principal.grants.push_back({scope, right});
        }
        users.emplace(std::move(id), std::move(principal));
    }
};

// Only the simulator uses memory storage. Tests reuse it across service instances
// to exercise the recovery protocol without a live M09 adapter.
class MemoryJournal final : public JournalPort {
public:
    std::optional<Journal> saved;
    bool offline{}, bad_confirmation{};

    Load load(const Scope& requested) override {
        if (offline || requested != scope) {
            return {};
        }
        return {saved ? LoadStatus::Restored : LoadStatus::Empty, saved};
    }

    Confirmation commit(std::uint64_t expected, const Journal& next) override {
        if (offline || expected != (saved ? saved->revision : 0) || next.revision != expected + 1 ||
            next.scope != scope) {
            return {};
        }
        saved = next;
        return {true, next.revision, bad_confirmation ? "different-operation" : next.transaction};
    }
};

class Cell final : public WorkCellPort {
public:
    Tick& now;
    Runtime runtime;
    bool effects_enabled{true}, unknown_delivery{}, simulate_bad_package{true};
    std::vector<Command> submissions;

    explicit Cell(Tick& clock) : now(clock) {
        runtime.scope = scope;
        runtime.revision = runtime.epoch = 1;
        runtime.line = {LineMode::Idle, true, true, {}, stamp("PLC/E01")};
        runtime.safety = {true, stamp("P02/IF-007")};
        runtime.bins = {{"B01", "PET-01", {}, 1, 1, true, true, false, true, true, stamp("M07/箱位")}};
        const auto pkg = settings().catalog.at(std::string(baseline));
        runtime.active = Deployment{"initial", pkg.id, pkg.digest, 1, pkg.versions, stamp("M01–M07"), {}};
        runtime.tasks = {{"T042", "B01", "末端已脱离", "CARRIER-03", "返回中", {},
                          stamp("M07"), stamp("独立计量")}};
        runtime.alarms = {{"A-042", "T042 独立归宿待核", "可继续观察执行状态", true, stamp("独立计量")}};
        for (const auto& [module, version] : pkg.versions) {
            runtime.modules.push_back({module, "就绪", version, stamp(module)});
        }
    }

    Stamp stamp(std::string source) const { return {std::move(source), now, now + 20s, true}; }
    void changed() { ++runtime.revision; }
    void replace(std::string physical) {
        auto& bin = runtime.bins.front();
        bin.physical = std::move(physical);
        ++bin.placement;
        bin.stamp = stamp("M07/箱位");
        changed();
    }

    void refresh() {
        const auto extend = [&](Stamp& fact) {
            fact.observed = now;
            fact.valid_until = now + 20s;
        };
        extend(runtime.line.stamp);
        extend(runtime.safety.stamp);
        for (auto& bin : runtime.bins) extend(bin.stamp);
        for (auto& module : runtime.modules) extend(module.stamp);
        // The activation event retains its original evidence time. Extend only
        // its validity while the simulated modules continue reporting it.
        if (runtime.active) {
            runtime.active->stamp.valid_until = now + 20s;
            if (now - runtime.active->stamp.observed >= 10s) runtime.active->stamp.observed = now;
        }
    }

    std::optional<Runtime> read(const Scope& requested) override {
        return requested == scope ? std::optional(runtime) : std::nullopt;
    }

    Delivery submit(const Command& command) override {
        if (accepted_.contains(command.key)) return Delivery::Accepted;
        if (command.key.operation.scope != scope || command.runtime_revision != runtime.revision ||
            command.epoch != runtime.epoch) return Delivery::Rejected;
        const auto step = command.key.step;
        if (step == Step::FenceBin || step == Step::MapBin || step == Step::UnfenceBin) {
            const auto& bin = runtime.bins.front();
            if (command.object != bin.slot || command.fence != bin.fence ||
                command.placement != bin.placement || command.mapping != bin.mapping) return Delivery::Rejected;
        } else if (command.fence != runtime.line.fence) {
            return Delivery::Rejected;
        }
        submissions.push_back(command);
        accepted_.emplace(command.key, command);
        return unknown_delivery ? Delivery::Unknown : Delivery::Accepted;
    }

    Effect lookup(const CommandKey& key) override {
        if (auto done = effects_.find(key); done != effects_.end()) return done->second;
        const auto found = accepted_.find(key);
        if (found == accepted_.end() || !effects_enabled) return {key, EffectState::Pending, {}, {}};
        const auto& cmd = found->second;
        auto& line = runtime.line;
        auto& bin = runtime.bins.front();
        switch (key.step) {
        case Step::Start: line.mode = LineMode::Running; line.drained = false; break;
        case Step::Pause: line.mode = LineMode::Paused; break;
        case Step::Drain: line.mode = LineMode::Ended; line.drained = true; break;
        case Step::FenceBin: bin.fence = key.operation.id; bin.usable = false; break;
        case Step::MapBin: ++bin.mapping; break;
        case Step::UnfenceBin: bin.fence.clear(); bin.usable = true; bin.full = false; break;
        case Step::FenceLine: line.fence = key.operation.id; line.mode = LineMode::Paused; line.drained = true; break;
        case Step::StageConfig: {
            const auto& pkg = *cmd.package;
            runtime.staged = Deployment{key.operation.id, pkg.id, pkg.digest, cmd.deployment_epoch,
                                         pkg.versions, stamp("M01–M07"), {}};
            if (simulate_bad_package && pkg.id == broken) {
                runtime.staged->actual.erase("M03");
                runtime.staged->problems["M03"] = "HSI 波段顺序不符，期望 550→650 nm，实装 650→550 nm";
            }
            break;
        }
        case Step::ActivateConfig:
            runtime.active = runtime.staged;
            runtime.active->stamp = stamp("M01–M07");
            runtime.epoch = cmd.deployment_epoch;
            for (auto& module : runtime.modules) module.detail = runtime.active->actual.at(module.module);
            break;
        case Step::UnfenceLine: line.fence.clear(); break;
        }
        bin.stamp = stamp("M07/箱位");
        line.stamp = stamp("PLC/E01");
        changed();
        Effect effect{key, EffectState::Applied, stamp("模拟现场"), "demo/" + key.operation.id + "/" +
                      std::to_string(static_cast<int>(key.step))};
        effects_.emplace(key, effect);
        return effect;
    }

private:
    std::map<CommandKey, Command> accepted_;
    std::map<CommandKey, Effect> effects_;
};

class Archive final : public EvidencePort {
public:
    inline static const std::string csv = "target,planned_bin,actual_bin\nT041,B01,B01\nT042,B01,B02\n";
    inline static const std::string checksum = "sha256:bc9fdb8ae810af0b86818d36f4f014400a44a4eea19423c2dee7ccdb5d90556f";
    std::map<std::pair<std::string, std::string>, ExportJob> jobs;
    bool finish{};

    std::optional<EvidencePage> list(const EvidenceQuery& query, std::string_view reader) override {
        if (reader.empty() || query.scope != scope || query.manifest != manifest ||
            (!query.cursor.empty() && query.cursor != "T041")) return {};
        const EvidenceRow first{"T041", "B01", "B01", "计量证据 E-041"};
        const EvidenceRow second{"T042", "B01", "B02", "计量证据 E-042"};
        if (query.cursor == "T041") return EvidencePage{scope, query.manifest, {}, {second}, {"T043 尚未归档"}};
        if (query.limit == 1) return EvidencePage{scope, query.manifest, "T041", {first}, {}};
        return EvidencePage{scope, query.manifest, {}, {first, second}, {"T043 尚未归档"}};
    }

    std::optional<ExportJob> enqueue(const ExportRequest& request, std::string_view owner) override {
        if (request.manifest != manifest || request.first != "T041" || request.last != "T042" ||
            request.max_rows < 2) return {};
        auto [it, inserted] = jobs.try_emplace({std::string(owner), request.id},
            ExportJob{std::string(owner), request, ExportStage::Queued, {}, {}, {}});
        return inserted || it->second.request == request ? std::optional(it->second) : std::nullopt;
    }

    std::optional<ExportJob> find(const Scope&, std::string_view id, std::string_view owner) override {
        const auto job = jobs.find({std::string(owner), std::string(id)});
        if (job == jobs.end()) return {};
        if (finish) {
            job->second.stage = ExportStage::Ready;
            job->second.actual_range = "T041–T042";
            job->second.checksum = checksum;
        }
        return job->second;
    }

    std::optional<std::string> download(const ExportJob& job) override {
        return job.stage == ExportStage::Ready ? std::optional(csv) : std::nullopt;
    }
};

struct Fixture {
    Tick now{1s};
    Identity identity;
    Cell cell{now};
    MemoryJournal journal;
    Archive archive;
    HmiService hmi{settings(), identity, cell, journal, archive, [this] { return now; }};

    Fixture() {
        if (!hmi.recover()) throw std::runtime_error("could not initialize fixture");
    }

    Request request(std::string id, Action action, std::string object = {}, std::string value = {}) const {
        return {{scope, std::move(id)}, action, std::move(object), std::move(value),
                cell.runtime.revision, cell.runtime.epoch};
    }

    void drive(int ticks = 1) {
        while (ticks-- > 0) {
            now += 1s;
            cell.refresh();
            hmi.advance();
        }
    }

    Phase phase(std::string id) {
        const auto result = hmi.operations().query("auditor", {scope, std::move(id)});
        if (!result.operation) throw std::runtime_error("missing operation under test");
        return result.operation->phase;
    }
};

}  // namespace waste::m08::test
