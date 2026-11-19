#include "fixtures.h"
#include "m08_hmi/state_projection.h"

#include <array>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {
using namespace waste::m08;
namespace fixture = waste::m08::test;
using namespace std::string_view_literals;

constexpr std::array actions{"start"sv, "pause"sv, "drain"sv, "changeBin"sv, "approve"sv,
                             "publish"sv, "rollback"sv, "acknowledge"sv};
constexpr std::array phases{"accepted"sv, "waitingDevice"sv, "waitingClear"sv, "canChange"sv,
                            "waitingMapping"sv, "waitingActivation"sv, "applied"sv, "failed"sv, "needsReview"sv};
constexpr std::array issues{"none"sv, "notReady"sv, "unauthorized"sv, "invalid"sv, "staleView"sv,
                            "conflict"sv, "conditions"sv, "persistence"sv, "deviceRejected"sv, "effectUnknown"sv,
                            "loadFailed"sv, "capacity"sv, "notFound"sv};
constexpr std::array modes{"idle"sv, "running"sv, "paused"sv, "draining"sv, "ended"sv};
constexpr std::array exports{"queued"sv, "ready"sv, "failed"sv};

std::string q(std::string_view value) {
    std::string result{"\""};
    constexpr std::string_view hex = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c < 32) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else {
            if (c == '"' || c == '\\') result += '\\';
            result += static_cast<char>(c);
        }
    }
    return result + '"';
}

std::string b(bool value) { return value ? "true" : "false"; }
std::string n(std::uint64_t value) { return q(std::to_string(value)); }

std::string obj(std::initializer_list<std::pair<std::string_view, std::string>> parts) {
    std::string result{"{"};
    for (const auto& [key, value] : parts) {
        if (result.size() > 1) result += ',';
        result += q(key) + ':' + value;
    }
    return result + '}';
}

template<class Range, class Format>
std::string list(const Range& range, Format format) {
    std::string result{"["};
    for (const auto& value : range) {
        if (result.size() > 1) result += ',';
        result += format(value);
    }
    return result + ']';
}

std::string strings(const std::map<std::string, std::string>& values) {
    std::string result{"{"};
    for (const auto& [key, value] : values) {
        if (result.size() > 1) result += ',';
        result += q(key) + ':' + q(value);
    }
    return result + '}';
}

std::string encode(const Stamp& s, fixture::Fixture& f) {
    return obj({{"source", q(s.source)}, {"observed", n(s.observed.count())},
                {"fresh", b(fresh(s, f.now, f.hmi.operations().settings().max_fact_age))}});
}

std::string encode(const Operation& row) {
    return obj({{"id", q(row.request.key.id)}, {"actor", q(row.actor)},
                {"action", q(actions.at(static_cast<std::size_t>(row.request.action)))},
                {"phase", q(phases.at(static_cast<std::size_t>(row.phase)))},
                {"issue", q(issues.at(static_cast<std::size_t>(row.issue)))},
                {"object", q(row.request.object)}, {"value", q(row.request.value)},
                {"created", n(row.created.count())}, {"updated", n(row.updated.count())},
                {"evidence", list(row.evidence, q)}});
}

std::string encode(const Workbench& view, fixture::Fixture& f, std::string_view actor) {
    const auto& r = view.runtime;
    const auto deployment = [&](const std::optional<Deployment>& d) {
        return d ? obj({{"package", q(d->package)}, {"epoch", n(d->epoch)}, {"digest", q(d->digest)},
                        {"actual", strings(d->actual)}, {"problems", strings(d->problems)},
                        {"stamp", encode(d->stamp, f)}}) : "null";
    };
    const auto principal = f.identity.current(actor).value();
    return obj({
        {"station", q(r.scope.station)}, {"run", q(r.scope.run)}, {"revision", n(r.revision)},
        {"epoch", n(r.epoch)}, {"session", q(actor)}, {"operational", b(view.operational)},
        {"line", obj({{"mode", q(modes.at(static_cast<std::size_t>(r.line.mode)))}, {"ready", b(r.line.ready)},
                      {"drained", b(r.line.drained)}, {"fence", q(r.line.fence)}, {"stamp", encode(r.line.stamp, f)}})},
        {"safety", obj({{"permits", b(r.safety.permits)}, {"stamp", encode(r.safety.stamp, f)}})},
        {"bins", list(r.bins, [&](const Bin& bin) {
            return obj({{"slot", q(bin.slot)}, {"physical", q(bin.physical)}, {"fence", q(bin.fence)},
                        {"mapping", n(bin.mapping)}, {"usable", b(bin.usable)}, {"full", b(bin.full)},
                        {"present", b(bin.present)}, {"clear", b(bin.no_unresolved)},
                        {"stamp", encode(bin.stamp, f)}});
        })},
        {"tasks", list(r.tasks, [&](const TaskView& task) {
            return obj({{"target", q(task.target)}, {"planned", q(task.planned_bin)}, {"execution", q(task.stage)},
                        {"carrier", q(task.carrier)}, {"robot", q(task.robot)}, {"actual", q(task.destination)},
                        {"executionStamp", encode(task.execution, f)}, {"destinationStamp", encode(task.independent, f)}});
        })},
        {"alarms", list(r.alarms, [&](const Alarm& alarm) {
            return obj({{"id", q(alarm.id)}, {"description", q(alarm.description)}, {"impact", q(alarm.impact)},
                        {"active", b(alarm.active)}, {"stamp", encode(alarm.stamp, f)}});
        })},
        {"modules", list(r.modules, [&](const ModuleHealth& module) {
            return obj({{"module", q(module.module)}, {"status", q(module.status)}, {"detail", q(module.detail)},
                        {"stamp", encode(module.stamp, f)}});
        })},
        {"active", deployment(r.active)}, {"staged", deployment(r.staged)},
        {"packages", list(view.packages, [&](const Package& pkg) {
            return obj({{"id", q(pkg.id)}, {"digest", q(pkg.digest)}, {"e11", b(pkg.e11)},
                        {"approved", b(view.approvals.contains(pkg.id))}, {"expected", strings(pkg.versions)}});
        })},
        {"actions", list(view.actions, [&](const Eligibility& e) {
            return obj({{"action", q(actions.at(static_cast<std::size_t>(e.action)))}, {"object", q(e.object)},
                        {"issue", q(issues.at(static_cast<std::size_t>(e.issue)))}});
        })},
        {"operations", list(view.operations, [](const Operation& op) { return encode(op); })},
        {"acknowledgements", strings(view.acknowledgements)},
        {"audits", list(view.audits, [](const AuditEvent& entry) {
            return obj({{"id", q(entry.request.key.id)}, {"actor", q(entry.actor)},
              {"action", q(actions.at(static_cast<std::size_t>(entry.request.action)))},
              {"object", q(entry.request.object)}, {"revision", n(entry.request.revision)},
              {"epoch", n(entry.request.epoch)}, {"at", n(entry.at.count())}, {"issue", q(issues.at(static_cast<std::size_t>(entry.issue)))}});
        })},
        {"canReadEvidence", b(allows(principal, r.scope, Permission::Evidence))},
        {"canExport", b(allows(principal, r.scope, Permission::Export))}
    });
}

std::string encode(const EvidencePage& page) {
    return obj({{"manifest", q(page.manifest)}, {"next", q(page.next)}, {"gaps", list(page.gaps, q)},
        {"rows", list(page.rows, [](const EvidenceRow& row) {
            return obj({{"target", q(row.target)}, {"planned", q(row.planned_bin)}, {"actual", q(row.actual_bin)},
                        {"evidence", q(row.evidence)}});
        })}});
}

std::string encode(const ExportJob& job) {
    return obj({{"id", q(job.request.id)}, {"manifest", q(job.request.manifest)},
                {"stage", q(exports.at(static_cast<std::size_t>(job.stage)))},
                {"range", q(job.actual_range)}, {"checksum", q(job.checksum)}, {"gaps", list(job.gaps, q)}});
}

std::string reply(Issue issue, std::string content = "null") {
    return obj({{"issue", q(issues.at(static_cast<std::size_t>(issue)))}, {"data", std::move(content)}});
}

template<class T>
std::string reply(const Response<T>& result) {
    if constexpr (std::is_same_v<T, std::string>) {
        return reply(result.issue, result.data ? q(*result.data) : "null");
    } else {
        return reply(result.issue, result.data ? encode(*result.data) : "null");
    }
}

std::optional<std::uint64_t> number(std::string_view input) {
    std::uint64_t output{};
    auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), output);
    if (error != std::errc{} || end != input.data() + input.size()) return {};
    return output;
}

class Demo {
public:
    std::string command(const std::vector<std::string>& args) {
        if (args.empty()) return reply(Issue::Invalid);
        if (args[0] == "tick") {
            advance();
            return reply(Issue::None);
        }
        if (args.size() < 2 || !f.identity.current(args[1])) return reply(Issue::Unauthorized);
        const auto& verb = args[0];
        const auto& actor = args[1];
        if (verb == "view" && args.size() == 2) {
            const auto view = f.hmi.operations().workbench(actor);
            return view ? reply(Issue::None, encode(*view, f, actor)) : reply(Issue::NotReady);
        }
        if (verb == "submit" && args.size() == 8) {
            const auto action = std::ranges::find(actions, args[3]);
            const auto revision = number(args[6]);
            const auto epoch = number(args[7]);
            if (action == actions.end() || !revision || !epoch) return reply(Issue::Invalid);
            const Request request{{fixture::scope, args[2]}, static_cast<Action>(action - actions.begin()),
                                  args[4], args[5], *revision, *epoch};
            return operation(f.hmi.operations().submit(actor, request));
        }
        if (verb == "query" && args.size() == 3) {
            return operation(f.hmi.operations().query(actor, {fixture::scope, args[2]}));
        }
        if (verb == "page" && args.size() == 5) {
            const auto limit = number(args[4]);
            if (!limit || *limit > f.hmi.operations().settings().max_page) return reply(Issue::Invalid);
            return reply(f.hmi.evidence().list(actor, {fixture::scope, args[2], args[3], static_cast<std::size_t>(*limit)}));
        }
        if (verb == "export" && args.size() == 8) {
            const auto limit = number(args[7]);
            if (!limit || *limit > f.hmi.operations().settings().max_export_rows) return reply(Issue::Invalid);
            f.archive.finish = false;
            return reply(f.hmi.evidence().enqueue(actor, {fixture::scope, args[2], args[3], args[4], args[5],
                                                        args[6], static_cast<std::size_t>(*limit)}));
        }
        if (verb == "job" && args.size() == 3) return reply(f.hmi.evidence().status(actor, fixture::scope, args[2]));
        if (verb == "download" && args.size() == 3) return reply(f.hmi.evidence().download(actor, fixture::scope, args[2]));
        return reply(Issue::Invalid);
    }

private:
    fixture::Fixture f;
    std::map<OperationKey, Tick> can_change_since_;

    static std::string operation(const OperationReply& result) {
        return reply(result.issue, result.operation ? encode(*result.operation) : "null");
    }

    void advance() {
        using namespace std::chrono_literals;
        f.drive();
        f.archive.finish = true;
        for (const auto& [key, op] : f.journal.saved->operations) {
            if (op.phase != Phase::CanChange) {
                can_change_since_.erase(key);
                continue;
            }
            const auto [mark, _] = can_change_since_.try_emplace(key, f.now);
            const auto& bin = f.cell.runtime.bins.front();
            if (f.now - mark->second >= 3s && bin.physical != op.request.value) f.cell.replace(op.request.value);
        }
        if (f.cell.runtime.line.mode == LineMode::Running && f.now > 10s &&
            f.cell.runtime.tasks.front().destination.empty()) {
            auto& task = f.cell.runtime.tasks.front();
            task.destination = "B02";
            task.independent = f.cell.stamp("独立计量");
            f.cell.runtime.alarms.front().active = false;
            f.cell.changed();
        }
    }
};

}  // namespace

int main() {
    Demo demo;
    for (std::string line; std::getline(std::cin, line);) {
        try {
            std::istringstream input(line);
            std::vector<std::string> args;
            for (std::string arg; input >> std::quoted(arg);) args.push_back(std::move(arg));
            std::cout << demo.command(args) << std::endl;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            std::cout << reply(Issue::NotReady) << std::endl;
        }
    }
}
