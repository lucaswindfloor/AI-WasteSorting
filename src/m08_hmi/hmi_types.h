#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace waste::m08 {

using Tick = std::chrono::nanoseconds;
using Clock = std::function<Tick()>;

struct Scope {
    std::string station, run;
    auto operator<=>(const Scope&) const = default;
};

enum class Action { Start, Pause, Drain, ChangeBin, Approve, Publish, Rollback, Acknowledge };
enum class Permission { View, Operate, ChangeBin, Approve, Publish, Audit, Evidence, Export };
enum class LineMode { Idle, Running, Paused, Draining, Ended };
enum class Phase { Accepted, WaitingDevice, WaitingClear, CanChange, WaitingMapping,
                   WaitingActivation, Applied, Failed, NeedsReview };
enum class Step { Start, Pause, Drain, FenceBin, MapBin, UnfenceBin,
                  FenceLine, StageConfig, ActivateConfig, UnfenceLine };
enum class Issue { None, NotReady, Unauthorized, Invalid, StaleView, Conflict,
                   Conditions, Persistence, DeviceRejected, EffectUnknown, LoadFailed, Capacity, NotFound };

struct Grant {
    Scope scope;
    Permission permission{};
};

struct Principal {
    std::string subject;
    std::vector<Grant> grants;
};

struct Stamp {
    std::string source;
    Tick observed{}, valid_until{};
    bool valid{};
};

struct Line {
    LineMode mode{LineMode::Idle};
    bool ready{}, drained{};
    std::string fence;
    Stamp stamp;
};

struct SafetyMirror {
    bool permits{};
    Stamp stamp;
};

struct Bin {
    std::string slot, physical, fence;
    std::uint64_t placement{}, mapping{};
    bool present{}, usable{}, full{}, no_unresolved{}, change_permitted{};
    Stamp stamp;
};

struct Package {
    std::string id, digest;
    std::map<std::string, std::string> versions;
    bool e11{};
};

struct Deployment {
    std::string operation, package, digest;
    std::uint64_t epoch{};
    std::map<std::string, std::string> actual;
    Stamp stamp;
    std::map<std::string, std::string> problems;
};

struct TaskView {
    std::string target, planned_bin, stage, carrier, robot, destination;
    Stamp execution, independent;
};

struct Alarm {
    std::string id, description, impact;
    bool active{};
    Stamp stamp;
};

struct ModuleHealth {
    std::string module, status, detail;
    Stamp stamp;
};

struct Runtime {
    Scope scope;
    std::uint64_t revision{}, epoch{};
    Line line;
    SafetyMirror safety;
    std::vector<Bin> bins;
    std::optional<Deployment> staged, active;
    std::vector<TaskView> tasks;
    std::vector<Alarm> alarms;
    std::vector<ModuleHealth> modules;
};

struct OperationKey {
    Scope scope;
    std::string id;
    auto operator<=>(const OperationKey&) const = default;
};

struct Request {
    OperationKey key;
    Action action{};
    std::string object, value;
    std::uint64_t revision{}, epoch{};
    bool operator==(const Request&) const = default;
};

struct CommandKey {
    OperationKey operation;
    Step step{};
    auto operator<=>(const CommandKey&) const = default;
};

struct Command {
    CommandKey key;
    std::string object, value, fence;
    std::uint64_t runtime_revision{}, epoch{}, placement{}, mapping{};
    std::optional<Package> package;
    std::uint64_t deployment_epoch{};
};

struct Operation {
    Request request;
    std::string actor;
    Phase phase{Phase::Accepted};
    Issue issue{Issue::None};
    Tick created{}, updated{};
    std::string old_physical, prior_fence;
    std::uint64_t old_placement{}, old_mapping{}, deployment_epoch{};
    std::optional<Step> last_applied;
    std::optional<Command> pending;
    Tick issued{};
    std::vector<std::string> evidence;
};

struct AuditEvent {
    Request request;
    std::string actor;
    Issue issue{};
    Tick at{};
};

struct Journal {
    Scope scope;
    std::uint64_t revision{}, highest_epoch{};
    std::string transaction;
    std::map<OperationKey, Operation> operations;
    std::map<std::string, std::string> approvals;
    std::map<std::uint64_t, Deployment> activated;
    std::map<std::string, std::string> acknowledgements;
    std::vector<AuditEvent> audit;
};

struct Settings {
    Scope scope;
    std::string writer;
    std::map<std::string, Package> catalog;
    std::string approved_baseline;
    std::size_t max_operations{}, max_page{}, max_export_rows{};
    Tick max_fact_age{}, command_timeout{};
};

struct Transition {
    Phase phase;
    Issue issue{Issue::None};
    std::optional<Step> next{};
    bool record_activation{};
};

struct OperationReply {
    Issue issue{Issue::None};
    std::optional<Operation> operation;
};

struct Eligibility {
    Action action{};
    std::string object;
    Issue issue{Issue::None};
};

struct Workbench {
    Runtime runtime;
    bool line_fresh{}, safety_fresh{}, production_fenced{}, operational{};
    std::vector<std::string> stale_bins;
    std::vector<Eligibility> actions;
    std::vector<Operation> operations;
    std::map<std::string, std::string> approvals;
    std::vector<Package> packages;
    std::map<std::string, std::string> acknowledgements;
    std::vector<AuditEvent> audits;
};

}  // namespace waste::m08
