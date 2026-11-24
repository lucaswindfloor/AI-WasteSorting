#include "pick_replay.h"

#include "m04_tracking/tracking_service.h"
#include "m06_scheduling/scheduling_service.h"
#include "m07_execution/command_adapter.h"
#include "m07_execution/execution_service.h"
#include "m09_archive/adapters/m07_intent_store_adapter.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace waste::m01sim {
namespace {
using namespace std::chrono_literals;
using m01::Tick;
using m04::Point;
using m07::EventKind;

constexpr Tick horizon = 3200ms;
constexpr double depth_pitch_x = pet_depth_period.count(); // 1 m/s in the single-bottle scenarios.
constexpr double cup_radius_mm = 7.5;
const m05::ToolIdentity cup{"sim-arm", "sim-cup", "sim-tool-v1", "sim-tcp-v1"};
const m05::BinIdentity test_bin{"sim-bin-slot", "sim-unknown-bin"};
const m07::ControllerIdentity controller_id{"sim-e01", "sim-e01-session"};
constexpr std::string_view terminal = "sim-terminal";
constexpr std::string_view terminal_session = "sim-terminal-session";
constexpr std::string_view coverage_id = "sim-e05-4mm-v1";

std::string point(const Point& p) {
    return "[" + std::to_string(p.x) + "," + std::to_string(p.y) + "," + std::to_string(p.z) + "]";
}

std::string ms(Tick time) {
    return std::to_string(std::chrono::duration<double, std::milli>(time).count());
}

std::string count(long double value) { return std::to_string(value); }

Point area_center(const m04::Footprint& f) {
    Point center{};
    // The production RGB rule uses a uniform 4 mm raster. Unique foreground samples carry equal area.
    for (const auto& p : f.points) {
        center.x += p.position.x;
        center.y += p.position.y;
    }
    center.x /= f.points.size();
    center.y /= f.points.size();
    return center;
}

double scene_centroid_x(const Material& material) {
    const auto profile = footprint(material);
    double area = 0, first_moment = 0;
    for (std::size_t i = 1; i < profile.size(); ++i) {
        const auto& left = profile[i - 1];
        const auto& right = profile[i];
        const auto length = right.along - left.along;
        const auto strip = length * (left.width_fraction + right.width_fraction) / 2;
        area += strip;
        first_moment += left.along * strip + length * length * (left.width_fraction + 2 * right.width_fraction) / 6;
    }
    return material.x + material.length * (first_moment / area - 0.5);
}

m04::TrackingConfig tracking_config(const m02::Snapshot& s, const m03::ModelIdentity& model) {
    m04::TrackingConfig config;
    config.generation = {s.run_id, s.generation};
    config.calibration = s.versions;
    config.encoder = s.encoder;
    config.models = {{model, m03::ScoreKind::Score, 0.5f, {}}};
    config.limits = {4, 8, 16384, 8192, 8192, 2, 32};
    config.max_target_age = 3s;
    config.max_evidence_age = 3s;
    config.max_encoder_age = 25ms;
    config.max_clock_error = 2ms;
    config.max_association_distance_mm = 24;
    config.min_shared_fraction = 0.5;
    config.min_instance_points = 24;
    config.min_material_samples = 1;
    config.ownership_radius_mm = 6;
    config.min_material_fraction = 0.75;
    config.max_surface_gap_mm = 6;
    config.min_surface_height_mm = 1;
    config.max_prediction_distance_mm = 3400;
    config.max_prediction_error_mm = 8;
    return config;
}

m05::Recipe recipe(const m03::Generation& generation, double depth_pitch_y) {
    m05::Recipe result;
    result.generation = generation;
    result.version = "sim-pick-v1";
    result.bin_mapping_version = "sim-bin-map-v1";
    result.coverage = {std::string(coverage_id), depth_pitch_x, depth_pitch_y, 0.1, 1.25};
    result.tools.emplace(cup, m05::ToolRule{cup, cup_radius_mm, 2, 0.5, 7, {}});
    result.unknown = m05::UnknownRule{{m04::MaterialStatus::Insufficient},
                                     {"sim-unknown", {cup}, {test_bin}, {}, false}};
    result.limits = {8192, 128, 1, 1, 4, 4, 256};
    result.valid_until = horizon;
    result.max_fact_age = horizon;
    return result;
}

m06::SchedulingConfig scheduling(const m03::Generation& g, const Layout& belt) {
    m06::SchedulingConfig result;
    result.generation = g;
    result.version = "sim-schedule-v1";
    result.scheduler_session = "sim-scheduler";
    result.valid_until = horizon;
    result.max_prediction_age = 25ms;
    result.transaction_lead = 3ms;
    result.max_tracking_error_mm = 8;
    result.max_attempts = 4;
    result.max_batch = 4;
    result.max_resources = 4;
    result.max_intervals = 8;
    result.max_retries = 2;
    result.lanes = {{cup.station, "sim-robot", m06::Contract::B, m06::TrackingMode::ControllerNative,
                     {{"sim-pick-zone", {belt.pick_begin, 0, 0}, {belt.pick_end, belt.width, 100}}},
                     {0ms, 12ms, 85ms, 90ms, 2ms, 4ms}, {0ms, 310ms}, {0ms, 310ms}, {0ms, 310ms}}};
    return result;
}

m07::ExecutionConfig execution_config(const m03::Generation& g) {
    return {g, "sim-execution-v1", "sim-execution", "sim-plc", "sim-safety-mirror",
            horizon, horizon, 300ms, 100ms, 4, 32, 512, 4, 16,
            {{cup.station, "sim-robot", controller_id.device, std::string(terminal), m06::Contract::B,
              m06::TrackingMode::ControllerNative, 5ms, false}}};
}

m07::FieldState field(const m03::Generation& g) {
    const auto calendar = [](m06::ResourceKind kind, std::string name) {
        return m06::ResourceCalendar{{kind, std::move(name)}, horizon, {{0ns, horizon}}, {}};
    };
    return {{g, 1, "sim-bin-map-v1",
             {{cup, m05::ToolCondition::Ready, {"sim-e01", 0ns, horizon}}},
             {{test_bin, 1, m05::BinCondition::Ready, {"sim-plc", 0ns, horizon}}}},
            {calendar(m06::ResourceKind::Robot, "sim-robot"), calendar(m06::ResourceKind::Tool, cup.tool),
             calendar(m06::ResourceKind::Bin, test_bin.physical_bin)},
            {{controller_id.device, controller_id.session, 0ns, horizon, true}},
            {{std::string(terminal), std::string(terminal_session), 0ns, horizon, true}},
            {"sim-plc", 0ns, horizon, true}, {"sim-safety-mirror", 0ns, horizon, true}, 1, 0ns};
}

// The synthetic depth profile certifies small cells on the continuous bottle body. Only actual
// calibrated E05 samples may populate cells; missing measurements cannot acquire proof by interpolation.
void prepare_surface(m06::CurrentTarget& current, double pitch_y) {
    const auto& target = current.target;
    const auto center = area_center(target.footprint);
    const m04::Sample* seed = nullptr;
    double best = 12;
    for (const auto& patch : target.surfaces) {
        for (const auto& sample : patch.points) {
            const auto distance = std::hypot(sample.position.x - center.x, sample.position.y - center.y);
            if (distance < best) {
                best = distance;
                seed = &sample;
            }
        }
    }
    if (!seed) return;
    constexpr int rows = 7, cols = 9;
    const double sx = seed->position.x, sy = seed->position.y;
    m05::SurfaceGrid grid{m05::binding(target), "sim-e05-body", std::string(coverage_id),
                         sx - rows * depth_pitch_x / 2, sy - cols * pitch_y / 2,
                         rows, cols, std::vector<m05::Cell>(rows * cols), target.valid_until};
    for (const auto& patch : target.surfaces) {
        for (const auto& sample : patch.points) {
            const auto x = static_cast<int>(std::lround((sample.position.x - sx) / depth_pitch_x)) + rows / 2;
            const auto y = static_cast<int>(std::lround((sample.position.y - sy) / pitch_y)) + cols / 2;
            if (x < 0 || x >= rows || y < 0 || y >= cols) continue;
            auto& cell = grid.cells[x * cols + y];
            if (std::abs(sample.position.x - (sx + (x - rows / 2) * depth_pitch_x)) <= 0.01 &&
                std::abs(sample.position.y - (sy + (y - cols / 2) * pitch_y)) <= 0.01 &&
                !cell.source && std::abs(sample.position.z - seed->position.z) <= 6) {
                cell = {m05::CellState::Observed, sample.source.key};
            }
        }
    }
    current.regions.push_back({"bottle-body", sx, sy, 0});
    current.grids.push_back(std::move(grid));
}

class SimController final : public m07::ControllerPort {
public:
    const Layout& belt;
    const Tick& now;
    m09::M07IntentStore& journal;
    bool uncertain;
    bool contact_ok{}, released{};
    unsigned sends{}, queries{};
    Tick sent{}, contact{};
    std::optional<m07::Command> active;
    Point actual_contact;

    SimController(const Layout& b, const Tick& clock, m09::M07IntentStore& store, bool lose_ack)
        : belt(b), now(clock), journal(store), uncertain(lose_ack) {}

    std::vector<std::byte> encode(const m07::Command& command) override {
        const auto data = std::to_string(command.request.attempt.sequence) + ":" + point(command.payload.pick) +
                          ":" + count(command.payload.reference.count);
        const auto bytes = std::as_bytes(std::span(data));
        return {bytes.begin(), bytes.end()};
    }

    m07::Transport transmit(const m07::WireCommand& wire) override {
        ++sends;
        const auto& c = wire.command;
        const auto saved = journal.lookup(c.controller, c.request);
        if (active || c.request.operation != m07::Operation::FinalGoal || !saved.value ||
            saved.value->wire != wire || saved.value->progress != m07::Progress::Armed) {
            return m07::Transport::DefinitelyNotSent;
        }
        active = c;
        sent = now;
        contact = sent + 91ms; // Receipt at +5 ms, contact 86 ms after receipt.
        return uncertain ? m07::Transport::Uncertain : m07::Transport::Submitted;
    }

    m07::QueryReply query(const m07::ControllerIdentity& original, const m07::RequestId& request) override {
        ++queries;
        return {request, original, m07::QueryState::Unknown, {}};
    }

    bool contacts_bottle(const Point& pick, double displacement) const {
        // Evaluate the suction footprint against the simulator's continuous surface,
        // independently of the sampled E05 grid used by M05 to propose the grasp.
        for (int dx = -8; dx <= 8; ++dx) {
            for (int dy = -8; dy <= 8; ++dy) {
                if (dx * dx + dy * dy > cup_radius_mm * cup_radius_mm) continue;
                const auto z = surface_height(belt, pick.x + dx, pick.y + dy, displacement);
                if (z <= 0 || std::abs(z - pick.z) > 4) return false;
            }
        }
        return true;
    }

    std::vector<std::pair<EventKind, bool>> due(std::int64_t actual_count, double millimeters_per_count) {
        if (!active || uncertain) return {};
        const auto& p = active->payload;
        if (now == sent + 5ms) return {{EventKind::Receipt, false}, {EventKind::Admitted, false}};
        if (now == contact) {
            const auto x = p.pick.x + static_cast<double>(actual_count - p.reference.count) * millimeters_per_count;
            const auto displacement = actual_count * millimeters_per_count;
            const auto height = surface_height(belt, x, p.pick.y, displacement);
            actual_contact = {x, p.pick.y, height};
            contact_ok = now >= p.contact.open && now <= p.contact.close &&
                         x >= belt.pick_begin && x <= belt.pick_end && p.bin == test_bin &&
                         height > 0 && std::abs(height - p.pick.z) <= 2 &&
                         contacts_bottle({x, p.pick.y, p.pick.z}, displacement);
            if (contact_ok) return {{EventKind::MotionObserved, false}, {EventKind::Holding, true}};
        }
        if (contact_ok && now == contact + 85ms) {
            released = true;
            return {{EventKind::PlacementEnded, false}, {EventKind::Released, true}};
        }
        if (contact_ok && now == contact + 130ms) {
            return {{EventKind::Finished, false}, {EventKind::ToolEmpty, true},
                    {EventKind::ToolHealthy, true}, {EventKind::RobotReady, false}};
        }
        return {};
    }
};

std::string_view event_name(EventKind event) {
    switch (event) {
    case EventKind::Receipt: return "receipt";
    case EventKind::Admitted: return "admitted";
    case EventKind::MotionObserved: return "contact";
    case EventKind::Holding: return "holding";
    case EventKind::PlacementEnded: return "placement-ended";
    case EventKind::Released: return "released";
    case EventKind::Finished: return "finished";
    case EventKind::ToolEmpty: return "tool-empty";
    case EventKind::ToolHealthy: return "tool-healthy";
    case EventKind::RobotReady: return "robot-ready";
    default: return "other";
    }
}

}  // namespace

struct PickReplay::Impl final : m06::TargetSource {
    Scenario scenario;
    const Layout& belt;
    double pitch_y;
    double expected_center_x;
    m04::TrackingConfig config;
    Log log;
    Tick now{};
    std::int64_t actual_count{};
    m04::TrackingService tracking;
    m09::M07IntentStore journal;
    SimController controller;
    m05::EligibilityService eligibility;
    m06::SchedulingService scheduler;
    m07::ExecutionService execution;
    std::optional<m04::TargetId> target_id;
    std::optional<m06::Task> task;
    unsigned complete_matches{}, partial_windows{}, feedback_sequence{};
    Tick last_match{};
    bool second_target{}, missing_surface{}, lost_prediction{}, position_checked{}, executed{}, pending_logged{}, released_resources{};
    std::optional<m05::Reason> last_eligibility;
    std::optional<m06::Reason> last_schedule;

    Impl(Scenario mode, const Layout& layout, unsigned columns, m09::ArchiveService& archive,
         const m02::Snapshot& snapshot, const m03::ModelIdentity& model, Log callback)
        : scenario(mode), belt(layout), pitch_y(belt.width / columns),
          expected_center_x(scene_centroid_x(layout.objects.front())), config(tracking_config(snapshot, model)),
          log(std::move(callback)), journal(archive, cup.station, snapshot.run_id),
          controller(belt, now, journal, scenario == Scenario::PetReceiptLoss),
          eligibility(recipe(config.generation, pitch_y)),
          scheduler(scheduling(config.generation, belt), eligibility, [&] { return now; }),
          execution(execution_config(config.generation), controller, journal, eligibility, *this, [&] { return now; }) {
        tracking.activate(config, now);
        if (!execution.recover() || execution.update_field(field(config.generation)).issue != m07::Issue::None) {
            throw std::runtime_error("single-bottle simulation could not initialize M07");
        }
    }

    std::shared_ptr<const m06::CurrentTarget> read(const m06::TargetKey& key, Tick time) override {
        if (key.run_id != config.generation.run_id) return {};
        auto target = tracking.target(key.id);
        if (!target) return {};
        const auto prediction = tracking.predict(target->id, target->version, time);
        auto result = std::make_shared<m06::CurrentTarget>(m06::CurrentTarget{
            std::move(*target), prediction, {config.generation, prediction.current, 1000, 1000,
                                             horizon, "sim-belt-1mps"}, {}, {}});
        prepare_surface(*result, pitch_y);
        return result;
    }

    void observe(const m03::Result& result) {
        if (result.state != m03::State::Completed) return;
        auto whole = result;
        std::erase_if(whole.evidence, [&](const auto& item) {
            const bool clipped = std::ranges::any_of(item.samples, [&](const auto& p) {
                const auto trigger = p.source.trigger.id;
                return trigger < result.window.first + 4 || trigger + 4 > result.window.last;
            });
            if (clipped) ++partial_windows;
            return clipped;
        });
        if (whole.evidence.empty()) {
            log(now, "pick-window", "{\"first\":" + std::to_string(result.window.first) +
                                     ",\"state\":\"partial\"}");
            return;
        }
        for (const auto& association : tracking.ingest_window(whole, now).associations) {
            if (!association.matched) continue;
            if (target_id && target_id != association.matched) second_target = true;
            target_id = association.matched;
            ++complete_matches;
            last_match = now;
            log(now, "pick-window", "{\"first\":" + std::to_string(result.window.first) +
                     ",\"state\":\"matched\",\"id\":" + std::to_string(*target_id) + "}");
        }
    }

    void schedule() {
        if (!target_id || task || now < last_match + 128ms || now >= horizon - 600ms) return;
        const auto snap = execution.snapshot();
        const auto current = read({config.generation.run_id, *target_id}, now);
        if (!snap || !current || current->prediction.reason != m04::Reason::None) return;
        const auto result = eligibility.evaluate(current->target, snap->eligibility, {current->grids, current->regions}, now);
        if (last_eligibility != result.reason) {
            const auto& grids = current->grids;
            const auto observed = grids.empty() ? 0 : std::ranges::count(grids.front().cells,
                m05::CellState::Observed, &m05::Cell::state);
            log(now, "pick-eligibility", "{\"reason\":" + std::to_string(static_cast<int>(result.reason)) +
                ",\"state\":\"" + (result.reason == m05::Reason::SurfaceMissing ? "depth-missing" :
                                    result.candidates.empty() ? "blocked" : "ready") + "\""
                ",\"observedCells\":" + std::to_string(observed) + "}");
            last_eligibility = result.reason;
        }
        missing_surface |= result.reason == m05::Reason::SurfaceMissing;
        if (result.candidates.empty()) return;
        const std::array batch{m06::QueuedCandidate{result.candidates.front(), 0, 1}};
        const auto scheduled = scheduler.dispatch(batch, execution, *this, now);
        if (!scheduled.task) {
            if (!scheduled.decisions.empty() && last_schedule != scheduled.decisions.front().reason) {
                last_schedule = scheduled.decisions.front().reason;
                log(now, "pick-schedule-blocked", "{\"reason\":" + std::to_string(static_cast<int>(*last_schedule)) + "}");
            }
            return;
        }
        task = scheduled.task;
        const auto& t = *task;
        log(now, "pick-task", "{\"id\":" + std::to_string(t.candidate.target.id) +
            ",\"version\":" + std::to_string(t.candidate.target.version) + ",\"attempt\":" + std::to_string(t.attempt.sequence) +
            ",\"point\":" + point(t.pick) + ",\"referenceCount\":" + count(t.reference.count) +
            ",\"epoch\":" + std::to_string(t.reference.encoder_epoch) +
            ",\"handoffMs\":" + ms(t.handoff_at) + ",\"openMs\":" + ms(t.contact.instants.open) +
            ",\"closeMs\":" + ms(t.contact.instants.close) + ",\"sourceSamples\":" +
            std::to_string(t.candidate.contact.sources.size()) + "}");
    }

    void track_position() {
        if (!target_id || executed) return;
        const auto target = tracking.target(*target_id);
        const auto predicted = target ? tracking.predict(*target_id, target->version, now) : m04::Prediction{};
        if (!target || predicted.reason != m04::Reason::None) {
            if (!lost_prediction) log(now, "pick-position", "{\"valid\":false}");
            lost_prediction = true;
            return;
        }
        auto center = area_center(target->footprint);
        const auto reference = center;
        center.x += predicted.applied_mm;
        position_checked |= now >= 1500ms &&
                            std::abs(center.x - expected_center_x - actual_count * config.encoder.signed_mm_per_count) < 8 &&
                            std::abs(center.y - belt.objects.front().y) < 4;
        log(now, "pick-position", "{\"valid\":true,\"id\":" + std::to_string(*target_id) +
            ",\"version\":" + std::to_string(target->version) + ",\"point\":" + point(center) +
            ",\"referencePoint\":" + point(reference) +
            ",\"referenceCount\":" + count(predicted.origin.count) + ",\"count\":" + count(predicted.current.count) +
            ",\"epoch\":" + std::to_string(predicted.current.encoder_epoch) +
            ",\"mmPerCount\":" + std::to_string(config.encoder.signed_mm_per_count) +
            ",\"errorMm\":" + std::to_string(predicted.error_mm) + "}");
    }

    void advance() {
        if (now % 10ms == 0ns) schedule();
        for (const auto& report : execution.advance()) {
            if (report.issue == m07::Issue::TransportUncertain && !pending_logged) {
                pending_logged = true;
                log(now, "pick-action", "{\"state\":\"pending\"}");
            }
        }
        for (const auto& [kind, tool_event] : controller.due(actual_count, config.encoder.signed_mm_per_count)) {
            const auto& c = *controller.active;
            const m07::Feedback feedback{c.request.attempt, tool_event ? std::nullopt : std::optional(c.request),
                {tool_event ? m07::Source::Terminal : m07::Source::Controller,
                 tool_event ? std::string(terminal) : controller_id.device,
                 tool_event ? std::string(terminal_session) : controller_id.session, ++feedback_sequence,
                 now, now + 300ms, std::string(event_name(kind))}, kind, {}};
            if (execution.ingest(feedback).issue != m07::Issue::None) {
                throw std::runtime_error("M07 rejected simulated controller feedback: " + std::string(event_name(kind)));
            }
            if (kind == EventKind::Holding) {
                executed = true;
                log(now, "pick-position", "{\"valid\":false}");
            }
            const auto location = kind == EventKind::MotionObserved ?
                ",\"point\":" + point(controller.actual_contact) +
                ",\"count\":" + std::to_string(actual_count) : std::string{};
            log(now, "pick-action", "{\"state\":\"" + std::string(event_name(kind)) + "\"" + location + "}");
            if (kind == EventKind::RobotReady) {
                const auto snapshot = execution.snapshot();
                released_resources = execution.status(c.request.attempt) == m06::AttemptStatus::ClosedFinished &&
                    snapshot && snapshot->reservations.empty() && std::ranges::all_of(snapshot->calendars,
                        [](const auto& calendar) { return calendar.occupied.empty(); });
                if (released_resources) log(now, "pick-action", "{\"state\":\"resources-released\"}");
            }
        }
        if (now % 10ms == 0ns) track_position();
    }
};

PickReplay::PickReplay(Scenario scenario, const Layout& layout, unsigned depth_width, m09::ArchiveService& archive,
                       const m02::Snapshot& snapshot, const m03::ModelIdentity& model, Log log)
    : impl_(std::make_unique<Impl>(scenario, layout, depth_width, archive, snapshot, model, std::move(log))) {}

PickReplay::~PickReplay() = default;

void PickReplay::encoder(Tick now, std::int64_t position) {
    auto& p = *impl_;
    p.now = now;
    p.actual_count = position;
    if (now % 10ms != 0ns || p.executed) return;
    if (p.scenario == Scenario::PetEncoderLoss && now >= 1350ms) {
        p.tracking.advance(now);
        return;
    }
    p.tracking.update_encoder({p.config.generation, {now, 0ns, p.config.encoder.epoch,
                               static_cast<long double>(position), p.config.encoder.lock_error_counts},
                               p.config.calibration.encoder, 1, true, true}, now);
}

void PickReplay::surfaces(Tick now, const m02::Result& result) {
    if (impl_->executed || !std::ranges::any_of(result.independent, [](const auto& row) {
            return row.source.header.id.source == m01::Source::E05 &&
                std::ranges::any_of(row.measured, [](const auto& p) {
                    return p.quality == m01::Quality::Valid && p.position.z > 1;
                });
        })) return;
    impl_->tracking.ingest_surfaces(result, now);
}

void PickReplay::window(Tick now, const m03::Result& result) {
    impl_->now = now;
    if (!impl_->executed) impl_->observe(result);
}

void PickReplay::advance(Tick now) {
    impl_->now = now;
    if (now < horizon) impl_->advance();
}

std::string PickReplay::verify() {
    auto& p = *impl_;
    if (p.second_target || p.complete_matches < 2 || p.partial_windows == 0) {
        return "single-bottle tracking did not retain one ID across complete and clipped windows (matches=" +
            std::to_string(p.complete_matches) + ", clipped=" + std::to_string(p.partial_windows) + ")";
    }
    if (p.scenario != Scenario::PetEncoderLoss && !p.position_checked) {
        return "tracked centroid could not be verified beyond the scan station";
    }
    if (p.scenario == Scenario::PetDepthLoss && (p.task || p.controller.sends || !p.missing_surface)) {
        return "missing depth was not reported or a grasp was attempted";
    }
    if (p.scenario == Scenario::PetEncoderLoss && (p.controller.sends || !p.lost_prediction)) {
        return "stale encoder did not invalidate the grasp position";
    }
    if (p.scenario == Scenario::PetReceiptLoss) {
        const auto snapshot = p.execution.snapshot();
        if (!p.task || !p.pending_logged || p.controller.sends != 1 || !p.controller.queries || p.released_resources ||
            !snapshot || snapshot->reservations.size() != 1 || snapshot->unresolved_targets.empty()) {
            return "uncertain send was not retained for query with resources held";
        }
    }
    if (p.scenario == Scenario::PetPick && (!p.task || p.controller.sends != 1 || !p.controller.contact_ok ||
        !p.controller.released || !p.released_resources)) {
        return "bottle did not complete a verified simulated pick and resource release (sends=" +
            std::to_string(p.controller.sends) + ", eligibility=" +
            std::to_string(static_cast<int>(p.last_eligibility.value_or(m05::Reason::None))) + ", schedule=" +
            std::to_string(static_cast<int>(p.last_schedule.value_or(m06::Reason::None))) + ")";
    }
    const auto state = p.execution.state();
    if (!state || (p.task && !p.journal.publish(state->revision).value)) return "M07 command journal could not be published";
    return {};
}

}  // namespace waste::m01sim
