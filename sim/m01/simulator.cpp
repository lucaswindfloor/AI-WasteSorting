#include "simulator.h"
#include "calibration.h"

#include "m01_acquisition/acquisition_service.h"
#include "m09_archive/adapters/m01_capture_adapter.h"
#include "m09_archive/evidence/evidence_index.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace waste::m01sim {
namespace {

using m01::Tick;
using m01::Source;
using namespace std::chrono_literals;

constexpr std::string_view run_id = "M01-SIM";
constexpr unsigned absent_line = 87;
constexpr unsigned late_line = 153;

std::string quote(std::string_view value) {
    std::string out = "\"";
    constexpr std::string_view hex = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + '"';
}

std::string object(std::initializer_list<std::pair<std::string_view, std::string>> fields) {
    std::string out = "{";
    for (const auto& [key, value] : fields) {
        if (out.size() != 1) out += ',';
        out += quote(key) + ':' + value;
    }
    return out + '}';
}

template<class Range, class Format>
std::string array(const Range& values, Format format) {
    std::string out = "[";
    for (const auto& value : values) {
        if (out.size() != 1) out += ',';
        out += format(value);
    }
    return out + ']';
}

std::string integers(const auto& values) {
    return array(values, [](auto value) { return std::to_string(value); });
}

double ms(Tick t) { return std::chrono::duration<double, std::milli>(t).count(); }
std::string time(Tick t) { return std::to_string(ms(t)); }
const char* yes(bool value) { return value ? "true" : "false"; }

std::string_view source_name(Source source) {
    switch (source) {
    case Source::Sync: return "SYNC";
    case Source::Rgb: return "RGB";
    case Source::Hsi: return "HSI";
    case Source::E05: return "E05";
    }
    return {};
}

std::string_view issue_name(m01::Issue issue) {
    switch (issue) {
    case m01::Issue::OldGeneration: return "OldGeneration";
    case m01::Issue::WrongSession: return "WrongSession";
    case m01::Issue::InvalidPacket: return "InvalidPacket";
    case m01::Issue::UnknownAssociation: return "UnknownAssociation";
    case m01::Issue::MissingSync: return "MissingSync";
    case m01::Issue::Late: return "Late";
    case m01::Issue::DuplicateConflict: return "DuplicateConflict";
    case m01::Issue::Capacity: return "Capacity";
    case m01::Issue::ConsumerBacklog: return "ConsumerBacklog";
    }
    return {};
}

std::string_view quality(m01::Quality value) {
    switch (value) {
    case m01::Quality::Valid: return "valid";
    case m01::Quality::Missing: return "missing";
    case m01::Quality::Invalid: return "invalid";
    case m01::Quality::Stale: return "stale";
    case m01::Quality::Unknown: return "unknown";
    }
    return {};
}

struct Session {
    unsigned base{}, count{};
    Tick start;
    m01::Config config;
};

enum class Input { Encoder, Sync, Rgb, Hsi, Depth, Advance };

struct Event {
    Tick at, exposure;
    Input input;
    std::vector<unsigned> lines;
    unsigned frame{};
    bool old{};
};

struct Row {
    unsigned trigger{}, local{};
    std::uint64_t generation{}, count{}, segment{};
    Tick synced;
    std::optional<m01::ObservationId> rgb, hsi;
    std::optional<Tick> rgb_received, hsi_received;
    m01::Quality rgb_quality{}, hsi_quality{};
    std::uint64_t archive_version{};
};

struct SeenDepth {
    m01::ObservationId id;
    double distance{};
    bool expected_mask{};
    unsigned valid_zeros{}, masked{};
};

struct PendingArchive {
    Tick due;
    m01::Output output;
};

struct Trace {
    Tick at;
    std::string kind, data;
};

m01::Config settings(const Options& options, unsigned generation, Tick interval) {
    const auto& p = options.profile;
    m01::Config c;
    c.run_id = run_id;
    c.generation = generation;
    c.encoder_epoch = generation;
    const auto base = generation * 10;
    c.sync = {"SYNC-SIM", base, "sim", "", {}, true};
    c.rgb = {"RGB-SIM", base + 1, "sim", "", {"RGB8","packed",p.rgb_width,p.packet_lines,{}, {}, {}}, true};
    c.hsi = {"HSI-SIM", base + 2, "sim", "", {"U16LE","packed",p.hsi_width,p.packet_lines,
                                             wavelengths(p.hsi_bands),{}, {}}, true};
    c.e05 = m01::SourceConfig{"E05-SIM", base + 3, "sim", "", {"HEIGHT_F32LE","packed",p.depth_width,1,{},
                                                            "virtual-belt","mm"}, true};
    const auto longest_packet = std::max(3 * p.rgb_width, 2 * p.hsi_width * p.hsi_bands) * p.packet_lines;
    c.match_timeout = interval * p.packet_lines + Tick(longest_packet * 2) + 3ms;
    c.segment_timeout = 6ms;
    c.source_stale_timeout = 35ms;
    c.max_pending = 128;
    c.recent_history = 512;
    c.max_segment_items = 16;
    c.max_inflight_segments = 128;
    c.max_segment_bytes = 8 * 1024 * 1024;
    c.max_packet_bytes = std::max(longest_packet, p.depth_width * 5);
    c.max_packet_items = p.packet_lines;
    c.buffer_capacity = 100 * 1024 * 1024;
    c.buffer_high_water = 80 * 1024 * 1024;
    c.contiguous_trigger_ids = true;
    c.first_expected_trigger = 1;
    return c;
}

m01::TriggerKey trigger_key(const Session& session, unsigned local) {
    return {session.config.sync.session, session.config.encoder_epoch, local};
}

m01::ObservationId id_for(const Session& session, Source source, unsigned local) {
    const auto* device = m01::source_config(session.config, source);
    const auto first_frame = source == Source::Rgb ? 7000u : 400u;
    const auto lines = device->format.rows;
    return {source, device->session, first_frame + (local - 1) / lines, (local - 1) % lines};
}

std::string describe_row(const std::optional<m01::Observation>& row) {
    if (!row) return "null";
    const auto& h = row->header;
    return object({{"frame", std::to_string(h.id.frame)}, {"row", std::to_string(h.id.row)},
        {"session", std::to_string(h.id.session)}, {"exposureClock", quote(h.source_time->clock_domain)},
        {"exposureMs", time(Tick(h.source_time->ticks))}, {"receivedMs", time(row->received)},
        {"bytes", std::to_string(h.bytes.size)}});
}

std::string decimal(double value) { return std::isfinite(value) ? std::to_string(value) : "null"; }

std::string point(const m02::ProjectedPoint& p, std::size_t column) {
    return "[" + decimal(p.position.x) + ',' + decimal(p.position.y) + ',' + decimal(p.position.z) +
           ',' + quote(quality(p.quality)) + ',' + std::to_string(column) + ']';
}

template<class Format>
std::string sample_columns(std::size_t width, Format format) {
    std::vector<std::size_t> columns;
    const auto limit = std::min<std::size_t>(width, preview_width);
    for (std::size_t i = 0; i < limit; ++i) columns.push_back((2 * i + 1) * width / (2 * limit));
    return array(columns, format);
}

std::string_view count_name(m02::CountBasis basis) {
    switch (basis) {
    case m02::CountBasis::TriggerLatch: return "trigger";
    case m02::CountBasis::SourceExposureLatch: return "exposure";
    case m02::CountBasis::Interpolation: return "interpolation";
    }
    return {};
}

std::string describe_alignment(const m02::AlignedObservation* row) {
    if (!row) return "null";
    const auto data = [&](const auto& pixels) {
        return sample_columns(pixels.size(), [&](auto i) { return point(pixels[i], i); });
    };
    const auto& identity = row->source.header.id;
    return object({{"frame", std::to_string(identity.frame)}, {"row", std::to_string(identity.row)},
        {"session", std::to_string(identity.session)}, {"receivedMs", time(row->source.received)},
        {"exposureMs", row->time ? time(row->time->value) : "null"},
        {"exposureErrorMs", row->time ? time(row->time->error) : "null"},
        {"referenceMs", row->reference_time ? time(row->reference_time->value) : "null"},
        {"count", row->count ? decimal(static_cast<double>(row->count->value)) : "null"},
        {"countBasis", row->count ? quote(count_name(row->count->basis)) : "null"},
        {"shiftMm", row->shift ? decimal(row->shift->mm) : "null"},
        {"shiftErrorMm", row->shift ? decimal(row->shift->error_mm) : "null"},
        {"issue", quote(row->issue)}, {"measured", data(row->measured)}, {"aligned", data(row->aligned)}});
}

struct SpectralSample {
    unsigned column{}, raw{};
    std::optional<double> corrected;
    std::string_view reason;
};

SpectralSample spectral_sample(const m02::AlignedObservation& row, unsigned width, unsigned bands) {
    const auto band = bands / 2;
    const auto& source = row.source;
    const auto bytes = std::span(*source.payload).subspan(source.header.bytes.offset, source.header.bytes.size);
    SpectralSample out;
    for (unsigned column = 0; column < width; ++column) {
        const auto index = 2 * (column * bands + band);
        const auto raw = std::to_integer<unsigned>(bytes[index]) + 256 * std::to_integer<unsigned>(bytes[index + 1]);
        if (column == 0 || raw > out.raw) {
            out.column = column;
            out.raw = raw;
        }
    }
    if (!row.spectrum) return out;
    const auto& spectrum = *row.spectrum;
    const auto index = out.column * bands + band;
    if (!spectrum.reason.empty()) out.reason = spectrum.reason;
    else if (index < spectrum.quality.size()) {
        if (spectrum.quality[index] == m02::Quality::Valid) {
            out.corrected = spectrum.samples[index];
        } else {
            switch (spectrum.sample_issues[index]) {
            case m02::SpectralIssue::InvalidReference: out.reason = "invalid_reference"; break;
            case m02::SpectralIssue::Saturated: out.reason = "saturated"; break;
            default: out.reason = "invalid_sample"; break;
            }
        }
    }
    return out;
}

class Simulation {
public:
    explicit Simulation(const Options& options)
        : opts_(options), interval_(1500000000LL / opts_.speed_mm_s), archive_(opts_.output / "archive"),
          capture_(archive_, "sim-host") {}

    Report execute();

private:
    const Options& opts_;
    Tick interval_;
    m01::AcquisitionService acq_;
    m02::CalibrationService m02_;
    std::vector<m01::TriggerEvent> sync_window_;
    std::map<unsigned, m02::CoverageStatus> coverage_;
    unsigned projected_depth_{}, e05_zero_points_{}, e05_masked_points_{}, spectrum_valid_{};
    m09::ArchiveService archive_;
    m09::M01CaptureAdapter capture_;
    std::vector<Session> sessions_;
    std::vector<Trace> trace_;
    std::map<unsigned, Row> ledger_;
    std::vector<SeenDepth> depth_;
    std::map<m01::Issue, unsigned> diagnostics_;
    std::deque<PendingArchive> pending_;
    std::vector<std::string> errors_;
    unsigned depth_expected_{}, segment_count_{}, archive_count_{}, peak_pending_{};
    std::size_t peak_bytes_{};
    Tick latest_{}, next_archive_{};

    void run_session(const Session& session, const Session* previous);
    void deliver(const Session& session, const Event& event);
    void accept(Tick now, m01::Output output);
    void calibrate(Tick now, const Session& session, const std::shared_ptr<const m01::Segment>& segment,
                   std::span<const m01::Diagnostic> diagnostics);
    double displacement(const Session& session, Tick exposure) const;
    Tick exposure_at(const Session& session, Source source, unsigned local) const;
    void archive_until(Tick now);
    void observe_health(Tick now);
    Report verify();
    void write(const Report& report, std::uint64_t version, std::size_t gaps);
    void log(Tick at, std::string_view kind, std::string data);
    void check(bool value, std::string problem);
    bool missing(unsigned id) const;
    bool late(unsigned id) const;
};

bool Simulation::missing(unsigned id) const {
    return id == absent_line && (opts_.scenario == Scenario::Missing || opts_.scenario == Scenario::Mixed);
}

bool Simulation::late(unsigned id) const {
    return id == late_line && (opts_.scenario == Scenario::Late || opts_.scenario == Scenario::Mixed);
}

void Simulation::log(Tick at, std::string_view kind, std::string data) {
    trace_.push_back({at, std::string(kind), std::move(data)});
    latest_ = std::max(latest_, at);
}

void Simulation::check(bool value, std::string problem) {
    if (!value && errors_.size() < 50) errors_.push_back(std::move(problem));
}

void Simulation::run_session(const Session& session, const Session* previous) {
    m02_.install(calibration(session.config, opts_.scenario, interval_));
    sync_window_.clear();
    acq_.start(session.config, session.start);
    log(session.start, previous ? "reconnect" : "start",
        object({{"generation", std::to_string(session.config.generation)},
                {"encoderEpoch", std::to_string(session.config.encoder_epoch)},
                {"syncSession", std::to_string(session.config.sync.session)}}));
    std::vector<Event> events;
    const auto duration = interval_ * session.count;
    const auto stop = session.start + duration + session.config.match_timeout + 12ms;
    for (auto at = session.start; at <= stop; at += 1ms) events.push_back({at, at, Input::Advance, {}});
    for (unsigned local = 1; local <= session.count; ++local) {
        const auto exposed = session.start + local * interval_;
        events.push_back({exposed, exposed, Input::Encoder, {local}});
        events.push_back({exposed + 350us, exposed, Input::Sync, {local}});
    }
    const auto group_size = opts_.profile.packet_lines;
    for (unsigned first = 1; first <= session.count; first += group_size) {
        const auto last = std::min(first + group_size - 1, session.count);
        const auto exposed = session.start + last * interval_;
        std::vector<unsigned> rgb, hsi;
        for (auto local = first; local <= last; ++local) {
            const auto global = session.base + local;
            rgb.push_back(local);
            if (late(global)) {
                const auto actual = session.start + local * interval_;
                events.push_back({actual + session.config.match_timeout + 4ms, actual, Input::Hsi, {local}});
            } else if (!missing(global)) {
                hsi.push_back(local);
            }
        }
        const auto rgb_delay = 520us + Tick(rgb.size() * opts_.profile.rgb_width * 6);
        const auto hsi_delay = ((first - 1) / group_size % 2 == 0 ? 160us : 900us) +
                              Tick(hsi.size() * opts_.profile.hsi_width * opts_.profile.hsi_bands * 4);
        events.push_back({exposed + rgb_delay, exposed, Input::Rgb, std::move(rgb)});
        if (!hsi.empty()) {
            const auto hsi_exposed = exposure_at(session, Source::Hsi, last);
            auto arrival = hsi_exposed + hsi_delay + hsi_transport_delay(opts_.scenario);
            if (opts_.scenario == Scenario::CountInterpolation && std::ranges::find(hsi, 100u) != hsi.end()) {
                arrival = std::max(arrival, session.start + 101 * interval_ + 351us);
            }
            events.push_back({arrival, hsi_exposed, Input::Hsi, std::move(hsi)});
        }
    }
    if (opts_.depth == DepthMode::Independent) {
        const auto belt_stops = duration + hsi_exposure_delay(opts_.scenario, interval_);
        for (auto elapsed = Tick(12ms); elapsed <= belt_stops; elapsed += 12ms) {
            const auto exposure = session.start + elapsed;
            events.push_back({exposure + 760us, exposure, Input::Depth, {}, ++depth_expected_});
        }
    } else {
        for (unsigned local = 8; local <= session.count; local += 8) {
            const auto exposure = session.start + local * interval_;
            events.push_back({exposure + 760us, exposure, Input::Depth, {local}, ++depth_expected_});
        }
    }
    if (previous) {
        const auto at = session.start + 1450us;
        events.push_back({at, previous->start + previous->count * interval_, Input::Rgb, {previous->count}, 0, true});
    }
    std::ranges::stable_sort(events, {}, &Event::at);
    for (const auto& event : events) {
        archive_until(event.at);
        deliver(event.old ? *previous : session, event);
        observe_health(event.at);
    }
    archive_until(stop);
    accept(stop, acq_.close(stop, trigger_key(session, session.count)));
    log(stop, "close", object({{"generation", std::to_string(session.config.generation)}}));
    observe_health(stop);
}

double Simulation::displacement(const Session& session, Tick exposure) const {
    const auto steps = static_cast<double>((exposure - session.start).count()) / interval_.count();
    return (session.base + steps) * pitch_mm;
}

Tick Simulation::exposure_at(const Session& session, Source source, unsigned local) const {
    return session.start + local * interval_ +
           (source == Source::Hsi ? hsi_exposure_delay(opts_.scenario, interval_) : 0ns);
}

void Simulation::deliver(const Session& session, const Event& event) {
    const auto& conf = session.config;
    if (event.input == Input::Advance) {
        accept(event.at, acq_.advance(event.at));
        return;
    }
    if (event.input == Input::Encoder || event.input == Input::Sync) {
        const auto local = event.lines.front();
        const auto count = local * encoder_counts_per_line;
        if (event.input == Input::Encoder) {
            log(event.at, "trigger", object({{"trigger", std::to_string(session.base + local)},
                {"local", std::to_string(local)}, {"count", std::to_string(count)},
                {"distance", std::to_string((session.base + local) * pitch_mm)},
                {"generation", std::to_string(conf.generation)}, {"epoch", std::to_string(conf.encoder_epoch)}}));
        } else {
            const m01::SyncInput sync{conf.generation, {trigger_key(session, local), count, 1,
                m01::SourceTime{"sim-encoder-clock", event.exposure.count()}}, event.at};
            log(event.at, "packet", object({{"source", quote("SYNC")}, {"triggers", integers(std::array{session.base + local})},
                                           {"bytes", "0"}, {"generation", std::to_string(conf.generation)}}));
            // These are the synchronized trigger events the simulator has actually delivered so far.
            sync_window_.push_back(sync.event);
            if (sync_window_.size() > 48) sync_window_.erase(sync_window_.begin());
            accept(event.at, acq_.ingest(sync, event.at));
        }
        return;
    }
    const auto source = event.input == Input::Rgb ? Source::Rgb : event.input == Input::Hsi ? Source::Hsi : Source::E05;
    const auto& device = *m01::source_config(conf, source);
    std::vector<m01::ObservationHeader> headers;
    std::vector<std::byte> bytes;
    std::vector<unsigned> triggers;
    const auto depth = source == Source::E05;
    const auto lane = depth ? std::vector{event.lines.empty() ? 0u : event.lines.front()} : event.lines;
    for (auto local : lane) {
        const auto acquired = depth || event.old ? event.exposure : exposure_at(session, source, local);
        const auto scan_at = source == Source::Hsi ? hsi_scan_position(opts_.scenario, interval_) : scan_x;
        auto signal = scan(source, opts_.profile, displacement(session, acquired),
                           depth && event.frame % 7 == 0, scan_at);
        m01::ObservationHeader h;
        h.id = depth ? m01::ObservationId{source, device.session, 8000 + event.frame, 0}
                     : id_for(session, source, local);
        if (local) {
            h.trigger = trigger_key(session, local);
            h.association_verified = true;
            triggers.push_back(session.base + local);
        }
        h.source_time = m01::SourceTime{device.device_id + "-clock", (acquired + clock_offset(source)).count()};
        h.time_basis = exposure_basis;
        if (source != Source::Hsi || hsi_has_native_count(opts_.scenario, session.base + local)) {
            const auto steps = static_cast<double>((acquired - session.start).count()) / interval_.count();
            h.encoder_count = std::llround(steps * encoder_counts_per_line);
            h.encoder_epoch = conf.encoder_epoch;
        }
        h.bytes = {bytes.size(), signal.data_bytes};
        if (signal.mask) h.valid_mask = m01::ByteRange{bytes.size() + signal.mask->offset, signal.mask->size};
        headers.push_back(std::move(h));
        bytes.insert(bytes.end(), signal.bytes.begin(), signal.bytes.end());
    }
    log(event.at, "packet", object({{"source", quote(source_name(source))}, {"triggers", integers(triggers)},
        {"bytes", std::to_string(bytes.size())}, {"rows", std::to_string(headers.size())},
        {"frame", std::to_string(headers.front().id.frame)}, {"generation", std::to_string(conf.generation)},
        {"old", yes(event.old)}}));
    const m01::PacketView packet{conf.generation, source, device.session, device.format, bytes, headers, event.at};
    accept(event.at, acq_.ingest(packet, event.at));
}

void Simulation::accept(Tick now, m01::Output output) {
    for (const auto& d : output.diagnostics) {
        ++diagnostics_[d.issue];
        unsigned global = 0;
        if (d.first_trigger) {
            const auto generation = d.first_trigger->encoder_epoch;
            if (generation && generation <= sessions_.size()) global = sessions_[generation - 1].base + d.first_trigger->id;
        }
        log(now, "diagnostic", object({{"issue", quote(issue_name(d.issue))}, {"source", quote(source_name(d.source))},
            {"trigger", std::to_string(global)}, {"hasEvidence", yes(d.evidence.has_value())}}));
    }
    for (const auto& segment : output.segments) {
        ++segment_count_;
        const auto& session = sessions_.at(segment->generation - 1);
        for (const auto& item : segment->items) {
            if (const auto* p = std::get_if<m01::TriggerRecord>(&item)) {
                const auto local = static_cast<unsigned>(p->event.key.id);
                const auto global = session.base + local;
                Row row{global, local, segment->generation, static_cast<std::uint64_t>(p->event.encoder_count),
                    segment->sequence, p->received, p->rgb ? std::optional(p->rgb->header.id) : std::nullopt,
                    p->hsi ? std::optional(p->hsi->header.id) : std::nullopt,
                    p->rgb ? std::optional(p->rgb->received) : std::nullopt,
                    p->hsi ? std::optional(p->hsi->received) : std::nullopt, p->rgb_quality, p->hsi_quality};
                check(ledger_.emplace(global, row).second, "duplicate M01 result for trigger " + std::to_string(global));
                log(now, "result", object({{"trigger", std::to_string(global)}, {"local", std::to_string(local)},
                    {"displacementMm", std::to_string(global * pitch_mm)},
                    {"hsiDisplacementMm", std::to_string(displacement(session, exposure_at(session, Source::Hsi, local)))},
                    {"count", std::to_string(row.count)}, {"generation", std::to_string(row.generation)},
                    {"segment", std::to_string(row.segment)}, {"syncReceivedMs", time(p->received)},
                    {"rgbQuality", quote(quality(p->rgb_quality))}, {"hsiQuality", quote(quality(p->hsi_quality))},
                    {"rgb", describe_row(p->rgb)}, {"hsi", describe_row(p->hsi)},
                    {"rgbPixels", p->rgb ? integers(preview(*p->rgb, opts_.profile)) : "[]"},
                    {"hsiPixels", p->hsi ? integers(preview(*p->hsi, opts_.profile)) : "[]"}}));
            }
            if (const auto* d = std::get_if<m01::Observation>(&item); d && d->header.id.source == Source::E05) {
                const auto pixels = preview(*d, opts_.profile);
                const auto zero = static_cast<unsigned>(std::ranges::count(pixels, 0));
                const auto mask = static_cast<unsigned>(std::ranges::count(pixels, -1));
                const auto exposure = d->received - 760us;
                const auto steps = static_cast<double>((exposure - session.start).count()) / interval_.count();
                const auto linked = d->header.trigger ? session.base + d->header.trigger->id : 0;
                const auto expected_mask = (d->header.id.frame - 8000) % 7 == 0;
                check(d->header.source_time == m01::SourceTime{"E05-SIM-clock", (exposure + 427us).count()} &&
                      (expected_mask == d->header.valid_mask.has_value()) &&
                      ((opts_.depth == DepthMode::Encoder && linked && (linked - session.base) % 8 == 0) ||
                       (opts_.depth == DepthMode::Independent && !linked)), "M01 E05 timing or validity mismatched");
                const auto displacement = (session.base + steps) * pitch_mm;
                depth_.push_back({d->header.id, displacement, expected_mask, zero, mask});
                log(now, "depth", object({{"frame", std::to_string(d->header.id.frame)},
                    {"session", std::to_string(d->header.id.session)}, {"trigger", std::to_string(linked)},
                    {"displacementMm", std::to_string(displacement)},
                    {"receivedMs", time(d->received)}, {"validZeros", std::to_string(zero)},
                    {"masked", std::to_string(mask)}, {"pixels", integers(pixels)}}));
            }
        }
        calibrate(now, session, segment, output.diagnostics);
    }
    if (output.segments.empty() && output.diagnostics.empty()) return;
    // This queue owns references until the actual archive call returns.
    const auto delay = opts_.scenario == Scenario::SlowArchive ? 22ms : 650us;
    next_archive_ = std::max(now, next_archive_) + delay;
    pending_.push_back({next_archive_, std::move(output)});
    peak_pending_ = std::max(peak_pending_, static_cast<unsigned>(pending_.size()));
}

void Simulation::calibrate(Tick now, const Session& session, const std::shared_ptr<const m01::Segment>& segment,
                           std::span<const m01::Diagnostic> diagnostics) {
    const auto& config = session.config;
    const m02::SyncWindow window{config.run_id, config.generation, sync_window_};
    const m02::Environment environment{config.run_id, config.generation, std::string(calibration_settings),
                                       session.start, 22, 5, true};
    std::vector<m02::ObservationReference> references;
    for (const auto& item : segment->items) {
        const auto* row = std::get_if<m01::Observation>(&item);
        if (!row || row->header.id.source != Source::E05 || !row->header.trigger) continue;
        const auto trigger = std::ranges::find(sync_window_, *row->header.trigger, &m01::TriggerEvent::key);
        if (trigger != sync_window_.end()) references.push_back({row->header.id, *trigger});
    }
    const auto result = m02_.process(segment, {&window, &environment, nullptr, diagnostics, references});
    if (!result.issue.empty() || !result.snapshot) {
        check(false, "M02 could not process generation " + std::to_string(config.generation) + ": " +
                     std::string(result.issue));
        return;
    }
    const auto& versions = result.snapshot->versions;
    for (const auto& group : result.triggers) {
        const auto global = static_cast<unsigned>(session.base + group.source.event.key.id);
        check(coverage_.emplace(global, group.coverage_status).second,
              "duplicate M02 result for trigger " + std::to_string(global));
        const auto* rgb = group.rgb ? &*group.rgb : nullptr;
        const auto* hsi = group.hsi ? &*group.hsi : nullptr;
        std::string spectrum = "null";
        if (hsi) {
            const auto sample = spectral_sample(*hsi, opts_.profile.hsi_width, opts_.profile.hsi_bands);
            if (sample.corrected) ++spectrum_valid_;
            spectrum = object({{"column", std::to_string(sample.column)}, {"raw", std::to_string(sample.raw)},
                {"corrected", sample.corrected ? decimal(*sample.corrected) : "null"},
                {"reason", quote(sample.reason)}, {"reference", quote(versions.spectral_reference)}});
            const bool should_correct = opts_.scenario != Scenario::InvalidWhite && opts_.scenario != Scenario::ClockMissing;
            check(sample.corrected.has_value() == should_correct &&
                  (!sample.corrected || std::abs(*sample.corrected - (sample.raw - 100.0) / 30000) < 1e-8),
                  "M02 spectral sample mismatch for trigger " + std::to_string(global));
            if (opts_.scenario != Scenario::ClockMissing) {
                const auto delay = hsi_exposure_delay(opts_.scenario, interval_);
                check(rgb && rgb->time && hsi->time && hsi->time->value - rgb->time->value == delay,
                      "M02 exposure time mismatch for trigger " + std::to_string(global));
                if (opts_.scenario != Scenario::CountInterpolation || global != trigger_count) {
                    const auto expected_shift = -pitch_mm * delay.count() / interval_.count();
                    check(hsi->shift && std::abs(hsi->shift->mm - expected_shift) <= 0.026,
                          "M02 along-belt shift mismatch for trigger " + std::to_string(global));
                }
            }
        }
        const auto pairs = sample_columns(group.common_coverage.size(), [&](auto i) {
            const auto& pair = group.common_coverage[i];
            return "[" + point(rgb->aligned[pair.rgb_column], pair.rgb_column) + ',' +
                         point(hsi->aligned[pair.hsi_column], pair.hsi_column) + ']';
        });
        log(now, "m02", object({{"trigger", std::to_string(global)}, {"generation", std::to_string(config.generation)},
            {"segment", std::to_string(segment->sequence)}, {"geometry", quote(versions.geometry)},
            {"clock", quote(versions.clock)}, {"encoder", quote(versions.encoder)},
            {"referenceMs", group.reference_time ? time(group.reference_time->value) : "null"},
            {"referenceCount", decimal(static_cast<double>(group.reference_count.value))},
            {"exposureLimitMs", time(result.snapshot->max_exposure_separation)},
            {"status", quote(name(group.coverage_status))}, {"coverage", std::to_string(group.common_coverage.size())},
            {"rgb", describe_alignment(rgb)}, {"hsi", describe_alignment(hsi)}, {"pairs", pairs}, {"spectrum", spectrum}}));
    }
    for (const auto& row : result.independent) {
        if (row.source.header.id.source != Source::E05) continue;
        const auto count = [&](auto predicate) { return std::ranges::count_if(row.measured, predicate); };
        const auto zeros = count([](const auto& p) { return p.quality == m02::Quality::Valid && p.position.z == 0; });
        const auto masked = count([](const auto& p) { return p.quality == m02::Quality::Missing; });
        const auto valid = count([](const auto& p) { return p.quality == m02::Quality::Valid; });
        const auto trigger = row.source.header.trigger ? session.base + row.source.header.trigger->id : 0;
        check(row.time && row.count && valid && (!trigger || row.shift),
              "M02 E05 projection/reference mismatch for frame " + std::to_string(row.source.header.id.frame));
        if (valid) ++projected_depth_;
        e05_zero_points_ += zeros;
        e05_masked_points_ += masked;
        log(now, "m02-depth", object({{"generation", std::to_string(config.generation)},
            {"trigger", std::to_string(trigger)}, {"geometry", quote(versions.geometry)}, {"valid", std::to_string(valid)},
            {"zeros", std::to_string(zeros)}, {"masked", std::to_string(masked)}, {"observation", describe_alignment(&row)}}));
    }
}

void Simulation::archive_until(Tick until) {
    while (!pending_.empty() && pending_.front().due <= until) {
        auto queued = std::move(pending_.front());
        pending_.pop_front();
        const auto stored = capture_.ingest(queued.output);
        if (!m09::accepted(stored.status) || !stored.value) {
            check(false, "M09 commit failed with status " + std::to_string(static_cast<int>(stored.status)));
        } else {
            archive_count_ += queued.output.segments.size();
            std::vector<unsigned> recorded;
            for (const auto& segment : queued.output.segments) {
                const auto& session = sessions_.at(segment->generation - 1);
                for (const auto& item : segment->items) {
                    if (const auto* row = std::get_if<m01::TriggerRecord>(&item)) {
                        const auto id = static_cast<unsigned>(session.base + row->event.key.id);
                        if (auto found = ledger_.find(id); found != ledger_.end()) {
                            found->second.archive_version = stored.value->version;
                        }
                        recorded.push_back(id);
                    }
                }
            }
            log(queued.due, "archive", object({{"manifest", std::to_string(stored.value->version)},
                {"triggers", integers(recorded)}, {"segments", std::to_string(archive_count_)},
                {"gaps", std::to_string(stored.value->gaps.size())}}));
        }
        queued.output = {};
        observe_health(queued.due);
    }
}

void Simulation::observe_health(Tick now) {
    const auto health = acq_.health();
    peak_bytes_ = std::max(peak_bytes_, health.payload_bytes);
    const auto data = object({{"ready", yes(health.ready)}, {"highWater", yes(health.high_water)},
        {"bytes", std::to_string(health.payload_bytes)}, {"pending", std::to_string(health.pending)},
        {"inflight", std::to_string(health.inflight_segments)}, {"archiveQueue", std::to_string(pending_.size())}});
    // Keep the report compact when adjacent receives have identical counters.
    if (!trace_.empty() && trace_.back().kind == "health" && trace_.back().data == data) return;
    log(now, "health", data);
}

Report Simulation::verify() {
    Report out;
    const auto current = archive_.manifest(run_id);
    const auto health = acq_.health();
    check(!health.payload_bytes && !health.inflight_segments && !health.pending && pending_.empty(),
          "M01 buffers were not released");
    check(ledger_.size() == trigger_count, "unexpected number of unique M01 trigger records");
    check(depth_.size() == depth_expected_, "E05 sample count mismatched");
    out.depth = static_cast<unsigned>(depth_.size());
    out.calibrated = static_cast<unsigned>(coverage_.size());
    out.covered = std::ranges::count_if(coverage_, [](const auto& trigger) {
        return trigger.second == m02::CoverageStatus::Available;
    });
    check(out.calibrated == trigger_count, "unexpected number of M02 trigger results");
    check(projected_depth_ == depth_expected_ && e05_zero_points_ && e05_masked_points_,
          "M02 E05 projections, valid zeros or masks were lost");
    out.late = diagnostics_[m01::Issue::Late];
    check(out.late == static_cast<unsigned>(late(late_line)), "late diagnostic count mismatched");
    for (const auto& [issue, count] : diagnostics_) {
        if (issue != m01::Issue::Late) {
            const auto expected = opts_.scenario == Scenario::Reconnect && issue == m01::Issue::OldGeneration ? 1u : 0u;
            check(count == expected, "unexpected M01 diagnostic " + std::string(issue_name(issue)));
        }
    }
    if (opts_.scenario == Scenario::Reconnect) {
        check(diagnostics_[m01::Issue::OldGeneration] == 1, "old-session packet was not rejected");
    }
    if (opts_.scenario == Scenario::SlowArchive) check(peak_pending_ > 1, "simulated archive queue did not grow");
    check(current.value.has_value(), "M09 manifest unavailable");
    if (!current.value) {
        write(out, 0, 0);
        return out;
    }
    const auto& manifest = *current.value;
    const m09::EvidenceIndex index(archive_, manifest);
    check(index.unavailable.empty(), "M09 index contains unreadable evidence records");
    check(archive_count_ == segment_count_, "one or more M01 segments were not acknowledged by M09");

    const auto depth_zeros = std::ranges::any_of(depth_, [](const auto& sample) { return sample.valid_zeros > 0; });
    const auto depth_mask = std::ranges::any_of(depth_, [](const auto& sample) { return sample.masked > 0; });
    check(depth_zeros && depth_mask, "E05 zero or invalid mask was lost");

    // Compare archive bytes and M01 row identities with independently generated
    // sensor expectations so an equal count can't hide a wrong association.
    const auto verify_lane = [&](const Session& session, Source source, unsigned local) {
        const auto id = id_for(session, source, local);
        const auto& device = *m01::source_config(session.config, source);
        const auto found = index.native.find({device.device_id, std::to_string(id.session), id.frame, id.row});
        if (found == index.native.end() || found->second.size() != 1) return false;
        const auto read = archive_.raw(manifest, found->second.front());
        const auto scan_at = source == Source::Hsi ? hsi_scan_position(opts_.scenario, interval_) : scan_x;
        const auto expected = scan(source, opts_.profile, displacement(session, exposure_at(session, source, local)),
                                   false, scan_at);
        if (!read.value || *read.value != expected.bytes) return false;
        ++out.verified;
        return true;
    };
    std::map<std::string, const m09::Record*> archived_triggers;
    for (const auto& [key, record] : index.records) {
        if (key.kind == m09::Kind::Trigger) {
            check(archived_triggers.emplace(key.id, &record).second, "duplicate trigger within archive");
        }
    }
    check(archived_triggers.size() == trigger_count, "unexpected number of M09 trigger records");

    for (const auto& session : sessions_) {
        for (unsigned local = 1; local <= session.count; ++local) {
            const auto global = session.base + local;
            const auto label = " at trigger " + std::to_string(global);
            const auto drop = missing(global);
            const auto delayed = late(global);
            const auto found = ledger_.find(global);
            if (found == ledger_.end()) {
                check(false, "M01 result missing" + label);
                continue;
            }
            const auto& actual = found->second;
            check(actual.count == static_cast<std::uint64_t>(local * encoder_counts_per_line) &&
                  actual.generation == session.config.generation &&
                  actual.rgb == id_for(session, Source::Rgb, local) && actual.rgb_quality == m01::Quality::Valid,
                  "M01 RGB/encoder association mismatched" + label);
            if (drop || delayed) {
                check(!actual.hsi && actual.hsi_quality == m01::Quality::Missing, "M01 HSI gap mismatched" + label);
            } else {
                check(actual.hsi == id_for(session, Source::Hsi, local) && actual.hsi_quality == m01::Quality::Valid,
                      "M01 HSI association mismatched" + label);
            }
            if (actual.hsi_quality == m01::Quality::Missing) ++out.missing;
            if (actual.rgb_quality == m01::Quality::Valid && actual.hsi_quality == m01::Quality::Valid) ++out.paired;

            using enum m02::CoverageStatus;
            auto expected = Available;
            if (drop || delayed) expected = MissingInput;
            else if (opts_.scenario == Scenario::ClockMissing) expected = UnverifiedTime;
            else if (opts_.scenario == Scenario::ExposureLimit) expected = ExposureSeparation;
            else if (opts_.scenario == Scenario::ExposureDelay) expected = OutOfCoverage;
            else if (opts_.scenario == Scenario::CountInterpolation && global == trigger_count) expected = UnverifiedCount;
            const auto status = coverage_.find(global);
            check(status != coverage_.end() && status->second == expected, "M02 common coverage mismatch" + label);

            const auto key = m09::capture_trigger_id(trigger_key(session, local));
            const auto saved = archived_triggers.find(key);
            check(saved != archived_triggers.end() && saved->second->raw.size() == (drop || delayed ? 1u : 2u) &&
                  saved->second->epoch == actual.generation && actual.archive_version != 0,
                  "M09 trigger references mismatched" + label);
            check(verify_lane(session, Source::Rgb, local), "M09 RGB bytes mismatched" + label);
            if (!drop) check(verify_lane(session, Source::Hsi, local), "M09 HSI bytes mismatched" + label);
        }
    }
    for (const auto& d : depth_) {
        const auto found = index.native.find({"E05-SIM", std::to_string(d.id.session), d.id.frame, d.id.row});
        const auto read = found != index.native.end() && found->second.size() == 1
                        ? archive_.raw(manifest, found->second.front()) : m09::Result<m09::Bytes>{};
        const auto expected = scan(Source::E05, opts_.profile, d.distance, d.expected_mask);
        check(read.value && *read.value == expected.bytes, "M09 E05 bytes mismatched for frame " +
              std::to_string(d.id.frame));
        if (read.value && *read.value == expected.bytes) ++out.verified;
    }
    check(manifest.gaps.size() == out.missing, "M09 gap count mismatched");
    for (const auto& gap : manifest.gaps) {
        check(gap.source == "HSI-SIM" && gap.basis == "trigger" && gap.range.end == gap.range.begin + 1 &&
              (missing(static_cast<unsigned>(gap.range.begin)) || late(static_cast<unsigned>(gap.range.begin))),
              "M09 recorded a gap for the wrong source or trigger");
    }

    out.passed = errors_.empty();
    write(out, manifest.version, manifest.gaps.size());
    return out;
}

void Simulation::write(const Report& report, std::uint64_t version, std::size_t gaps) {
    std::ranges::stable_sort(trace_, {}, &Trace::at);
    const auto& p = opts_.profile;
    const auto m02_summary = object({{"processed", std::to_string(report.calibrated)}, {"covered", std::to_string(report.covered)},
        {"depthProjected", std::to_string(projected_depth_)}, {"validZeroPoints", std::to_string(e05_zero_points_)},
        {"maskedPoints", std::to_string(e05_masked_points_)}, {"spectralValid", std::to_string(spectrum_valid_)}});
    const auto summary = object({{"passed", yes(report.passed)}, {"expected", std::to_string(trigger_count)},
        {"finalized", std::to_string(ledger_.size())}, {"paired", std::to_string(report.paired)},
        {"missing", std::to_string(report.missing)}, {"late", std::to_string(report.late)},
        {"rejectedOld", std::to_string(diagnostics_[m01::Issue::OldGeneration])},
        {"depth", std::to_string(report.depth)}, {"segments", std::to_string(archive_count_)},
        {"manifest", std::to_string(version)}, {"archiveGaps", std::to_string(gaps)},
        {"verifiedBlocks", std::to_string(report.verified)}, {"peakArchiveQueue", std::to_string(peak_pending_)},
        {"peakBytes", std::to_string(peak_bytes_)}, {"m02", m02_summary},
        {"errors", array(errors_, [](const auto& error) { return quote(error); })}});
    const auto meta = object({{"scenario", quote(name(opts_.scenario))}, {"speed", std::to_string(opts_.speed_mm_s)},
        {"pitch", std::to_string(pitch_mm)}, {"depthMode", quote(opts_.depth == DepthMode::Encoder ? "encoder" : "independent")},
        {"durationMs", time(latest_)}, {"beltWidth", std::to_string(belt_width)}, {"scanX", std::to_string(scan_x)},
        {"hsiScanX", std::to_string(hsi_scan_position(opts_.scenario, interval_))},
        {"travel", std::to_string(travel)}, {"previewWidth", std::to_string(preview_width)},
        {"previewTravel", decimal(travel + pitch_mm * hsi_exposure_delay(opts_.scenario, interval_).count() / interval_.count())},
        {"rgbWidth", std::to_string(p.rgb_width)}, {"hsiWidth", std::to_string(p.hsi_width)},
        {"hsiBands", std::to_string(p.hsi_bands)}, {"depthWidth", std::to_string(p.depth_width)},
        {"hsiPreviewBand", quote(wavelengths(p.hsi_bands)[p.hsi_bands / 2])},
        {"packetLines", std::to_string(p.packet_lines)},
        {"objects", array(materials, [](const Material& item) {
            return object({{"id", quote(item.id)}, {"material", quote(item.kind)}, {"x", std::to_string(item.x)},
                {"y", std::to_string(item.y)}, {"length", std::to_string(item.length)}, {"width", std::to_string(item.width)},
                {"footprint", array(footprint(item), [](const ProfileStation& station) {
                    return "[" + std::to_string(station.along) + "," + std::to_string(station.width_fraction) + "]";
                })}});
        })},
        {"motion", array(sessions_, [&](const Session& session) {
            const auto stop = exposure_at(session, Source::Hsi, session.count);
            return object({{"startMs", time(session.start)}, {"endMs", time(stop)},
                {"begin", std::to_string(session.base * pitch_mm)}, {"end", decimal(displacement(session, stop))}});
        })}});
    std::ofstream data(opts_.output / "report.json", std::ios::binary);
    data << "{\"schema\":1,\"meta\":" << meta << ",\"events\":[\n";
    bool first = true;
    for (const auto& row : trace_) {
        if (!first) data << ",\n";
        data << object({{"t", time(row.at)}, {"type", quote(row.kind)}, {"data", row.data}});
        first = false;
    }
    data << "\n],\"summary\":" << summary << "}\n";
    data.close();
    if (!data) throw std::runtime_error("could not write report.json");

    std::ofstream csv(opts_.output / "ledger.csv", std::ios::binary);
    csv << "trigger,local_trigger,generation,encoder_count,sync_received_ms,rgb_quality,rgb_frame,rgb_row,rgb_session,"
           "rgb_received_ms,hsi_quality,hsi_frame,hsi_row,hsi_session,hsi_received_ms,m01_segment,m09_manifest\n";
    const auto lane = [&](m01::Quality value, const auto& id, const auto& received) {
        csv << ',' << quality(value) << ',';
        if (id) csv << id->frame << ',' << id->row << ',' << id->session;
        else csv << ",,";
        csv << ',';
        if (received) csv << time(*received);
    };
    for (const auto& [id, row] : ledger_) {
        csv << id << ',' << row.local << ',' << row.generation << ',' << row.count << ',' << time(row.synced);
        lane(row.rgb_quality, row.rgb, row.rgb_received);
        lane(row.hsi_quality, row.hsi, row.hsi_received);
        csv << ',' << row.segment << ',' << row.archive_version << '\n';
    }
    csv.close();
    if (!csv) throw std::runtime_error("could not write ledger.csv");
}

Report Simulation::execute() {
    if (opts_.scenario == Scenario::Reconnect) {
        auto one = settings(opts_, 1, interval_);
        const auto count = trigger_count / 2;
        const auto next = interval_ * count + one.match_timeout + 20ms;
        sessions_.push_back({0, count, 0ns, std::move(one)});
        sessions_.push_back({count, trigger_count - count, next, settings(opts_, 2, interval_)});
    } else {
        sessions_.push_back({0, trigger_count, 0ns, settings(opts_, 1, interval_)});
    }
    for (std::size_t i = 0; i < sessions_.size(); ++i) {
        run_session(sessions_[i], i ? &sessions_[i - 1] : nullptr);
    }
    archive_until(Tick::max());
    const auto sealed = archive_.seal(run_id, false, "M01 simulation inputs and tail processed");
    check(m09::accepted(sealed.status), "M09 could not seal run");
    if (sealed.value) {
        log(latest_, "seal", object({{"manifest", std::to_string(sealed.value->version)}}));
    }
    return verify();
}

}  // namespace

std::string_view name(Scenario scenario) {
    switch (scenario) {
    case Scenario::Normal: return "normal";
    case Scenario::Missing: return "missing";
    case Scenario::Late: return "late";
    case Scenario::Mixed: return "mixed";
    case Scenario::SlowArchive: return "slow-archive";
    case Scenario::Reconnect: return "reconnect";
    case Scenario::TransportDelay: return "transport-delay";
    case Scenario::ExposureDelay: return "exposure-delay";
    case Scenario::Compensated: return "compensated";
    case Scenario::ExposureLimit: return "exposure-limit";
    case Scenario::ClockMissing: return "clock-missing";
    case Scenario::CountInterpolation: return "count-interpolation";
    case Scenario::InvalidWhite: return "invalid-white";
    }
    return {};
}

Report run(const Options& options) {
    if (options.output.has_parent_path()) std::filesystem::create_directories(options.output.parent_path());
    if (!std::filesystem::create_directory(options.output)) {
        throw std::runtime_error("output directory exists; choose a new path");
    }
    return Simulation(options).execute();
}

}  // namespace waste::m01sim
