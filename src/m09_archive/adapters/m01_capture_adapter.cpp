#include "m01_capture_adapter.h"
#include "checkpoint_layouts.h"

#include <algorithm>
#include <deque>

namespace waste::m09::wire {

template<> struct Layout<m01::SourceTime> {
    static void visit(auto& x, auto&& fields) { fields(x.clock_domain, x.ticks); }
};
template<> struct Layout<m01::TriggerKey> {
    static void visit(auto& x, auto&& fields) { fields(x.sync_session, x.encoder_epoch, x.id); }
};
template<> struct Layout<m01::TriggerEvent> {
    static void visit(auto& x, auto&& fields) { fields(x.key, x.encoder_count, x.direction, x.source_time); }
};
template<> struct Layout<m01::DataFormat> {
    static void visit(auto& x, auto&& fields) {
        fields(x.encoding, x.transport_layout, x.width, x.rows, x.bands, x.coordinates, x.units);
    }
};
template<> struct Layout<m01::ObservationHeader> {
    static void visit(auto& x, auto&& fields) {
        fields(x.id, x.trigger, x.association_verified, x.source_time, x.time_basis, x.encoder_count,
               x.encoder_epoch, x.source_quality, x.bytes, x.valid_mask, x.source_status);
    }
};

}  // namespace waste::m09::wire

namespace waste::m09 {
namespace {

const m01::SourceConfig* source(const m01::Config& config, m01::Source s) {
    switch (s) {
    case m01::Source::Sync: return &config.sync;
    case m01::Source::Rgb: return &config.rgb;
    case m01::Source::Hsi: return &config.hsi;
    case m01::Source::E05: return config.e05 ? &*config.e05 : nullptr;
    }
    return nullptr;
}

Time time(const std::optional<m01::SourceTime>& value) {
    return value ? Time{value->clock_domain, value->ticks} : Time{};
}

bool valid(m01::ByteRange range, std::size_t size) {
    return range.offset <= size && range.size <= size - range.offset;
}

std::string diagnostic_text(m01::Issue issue) {
    switch (issue) {
    case m01::Issue::OldGeneration: return "Old capture generation";
    case m01::Issue::WrongSession: return "Wrong source session";
    case m01::Issue::InvalidPacket: return "Invalid source packet";
    case m01::Issue::UnknownAssociation: return "Uncertain trigger association";
    case m01::Issue::MissingSync: return "Missing synchronization";
    case m01::Issue::Late: return "Late source evidence";
    case m01::Issue::DuplicateConflict: return "Conflicting capture duplicate";
    case m01::Issue::Capacity: return "Capture capacity exhausted";
    case m01::Issue::ConsumerBacklog: return "Capture consumer backlog";
    }
    return "Unknown capture diagnostic";
}

std::optional<Gap> missing(const m01::Config& config, const m01::Diagnostic& row) {
    if (row.issue != m01::Issue::MissingSync && row.issue != m01::Issue::Capacity &&
        row.issue != m01::Issue::ConsumerBacklog) return {};
    const auto* device = source(config, row.source);
    if (!device || !row.first_trigger) return {};
    const auto last = row.last_trigger.value_or(*row.first_trigger);
    if (last.sync_session != row.first_trigger->sync_session || last.encoder_epoch != row.first_trigger->encoder_epoch ||
        last.id < row.first_trigger->id || last.id == UINT64_MAX) return {};
    return Gap{device->device_id, std::to_string(device->session), "trigger",
               {row.first_trigger->id, last.id + 1}, diagnostic_text(row.issue)};
}

}  // namespace

std::string capture_trigger_id(const m01::TriggerKey& key) {
    return wire::key({std::to_string(key.sync_session), std::to_string(key.encoder_epoch), std::to_string(key.id)});
}

M01CaptureAdapter::M01CaptureAdapter(ArchiveService& archive, std::string receipt_domain)
    : archive_(archive), receipt_domain_(std::move(receipt_domain)) {
    if (!wire::identifier(receipt_domain_)) throw std::invalid_argument("invalid capture clock domain");
}

Result<Manifest> M01CaptureAdapter::ingest(const m01::Segment& segment) {
    if (!segment.config || segment.run_id != segment.config->run_id ||
        segment.generation != segment.config->generation || segment.sequence == UINT64_MAX) return {Status::Invalid, {}};
    const auto& config = *segment.config;
    std::map<m01::Source, SegmentInput> parts;
    std::map<m01::ObservationId, BlockRef> references;
    std::deque<Bytes> masked;
    std::vector<m01::Diagnostic> diagnostics;
    std::vector<Record> triggers;

    const auto add = [&](const m01::Observation& observation) -> std::optional<BlockRef> {
        auto header = observation.header;
        const auto& id = header.id;
        if (const auto old = references.find(id); old != references.end()) return old->second;
        const auto* device = source(config, id.source);
        if (!device || device->session != id.session || !observation.payload ||
            !valid(header.bytes, observation.payload->size()) || !header.bytes.size ||
            (header.valid_mask && !valid(*header.valid_mask, observation.payload->size()))) return {};
        auto [group, _] = parts.try_emplace(id.source, SegmentInput{
            {config.run_id, device->device_id, std::to_string(device->session),
             std::to_string(segment.generation) + "." + std::to_string(segment.sequence)},
            segment.generation, device->format.encoding, {}, {}});
        auto& input = group->second;
        auto bytes = std::span(*observation.payload).subspan(header.bytes.offset, header.bytes.size);
        header.bytes.offset = 0;
        if (header.valid_mask) {
            const auto mask = std::span(*observation.payload).subspan(header.valid_mask->offset, header.valid_mask->size);
            auto& packed = masked.emplace_back(bytes.begin(), bytes.end());
            header.valid_mask->offset = packed.size();
            packed.insert(packed.end(), mask.begin(), mask.end());
            bytes = std::span(packed);
        }
        wire::Encoder metadata;
        metadata.fields(wire::encode("m01-header-v1", header), device->format, device->firmware, device->sdk);
        const auto trigger = header.trigger && header.association_verified ? capture_trigger_id(*header.trigger) : "";
        Block block{id.frame, id.row, {}, trigger, time(header.source_time),
                    {receipt_domain_, observation.received.count()}, device->format.encoding, {},
                    wire::envelope("m01-block-v1", metadata.data)};
        const BlockRef ref{input.key, input.blocks.size()};
        input.blocks.push_back({std::move(block), bytes});
        references.emplace(id, ref);
        return ref;
    };

    for (const auto& item : segment.items) {
        if (const auto* row = std::get_if<m01::Observation>(&item)) {
            if (!add(*row)) return {Status::Invalid, {}};
        } else if (const auto* pair = std::get_if<m01::TriggerRecord>(&item)) {
            Record row;
            row.run = config.run_id;
            row.key = {Kind::Trigger, capture_trigger_id(pair->event.key), segment.sequence + 1};
            row.epoch = segment.generation;
            row.occurred = time(pair->event.source_time);
            row.received = {receipt_domain_, pair->received.count()};
            row.description = "Capture synchronization event";
            wire::Encoder details;
            details.fields(pair->event, pair->rgb_quality, pair->hsi_quality);
            row.detail = wire::envelope("m01-trigger-v1", details.data);
            for (const auto* observation : {&pair->rgb, &pair->hsi}) {
                if (!*observation) continue;
                const auto ref = add(**observation);
                if (!ref) return {Status::Invalid, {}};
                row.raw.push_back(*ref);
            }
            triggers.push_back(std::move(row));
            for (const auto s : {m01::Source::Rgb, m01::Source::Hsi}) {
                const bool rgb = s == m01::Source::Rgb;
                const auto quality = rgb ? pair->rgb_quality : pair->hsi_quality;
                if (quality != m01::Quality::Missing || pair->event.key.id == UINT64_MAX) continue;
                const auto* device = source(config, s);
                const auto result = archive_.gap(config.run_id, {device->device_id, std::to_string(device->session),
                    "trigger", {pair->event.key.id, pair->event.key.id + 1}, "No observation at trigger closure"});
                if (!accepted(result.status)) return result;
            }
        } else {
            diagnostics.push_back(std::get<m01::Diagnostic>(item));
        }
    }

    Result<Manifest> result = archive_.manifest(segment.run_id);
    for (const auto& [source_id, input] : parts) {
        result = archive_.ingest(input);
        if (!accepted(result.status)) return result;
    }
    if (!triggers.empty()) {
        result = archive_.append(config.run_id, triggers);
        if (!accepted(result.status)) return result;
    }
    return diagnostics.empty() ? result : diagnose(config, diagnostics);
}

Result<Manifest> M01CaptureAdapter::diagnose(const m01::Config& config,
                                               const std::vector<m01::Diagnostic>& diagnostics) {
    std::vector<Record> rows;
    for (const auto& row : diagnostics) {
        if (const auto gap = missing(config, row)) {
            const auto outcome = archive_.gap(config.run_id, *gap);
            if (!accepted(outcome.status)) return outcome;
        }
        wire::Encoder details;
        details.fields(row.issue, row.source, row.first_trigger, row.last_trigger, row.observation,
                       row.sync_evidence, row.input_generation);
        std::optional<BlockRef> raw;
        if (row.evidence && row.evidence->payload) {
            const auto& evidence = *row.evidence;
            auto header = evidence.header;
            const auto* device = source(config, header.id.source);
            const auto bytes = std::span(*evidence.payload);
            if (device && valid(header.bytes, bytes.size()) && header.bytes.size &&
                (!header.valid_mask || valid(*header.valid_mask, bytes.size()))) {
                const auto data = bytes.subspan(header.bytes.offset, header.bytes.size);
                const auto mask = header.valid_mask ?
                    bytes.subspan(header.valid_mask->offset, header.valid_mask->size) : std::span<const std::byte>{};
                Bytes packed(data.begin(), data.end());
                header.bytes.offset = 0;
                if (header.valid_mask) {
                    header.valid_mask->offset = packed.size();
                    packed.insert(packed.end(), mask.begin(), mask.end());
                }
                const auto format = device->session == header.id.session ? device->format.encoding : "opaque-capture";
                wire::Encoder metadata;
                metadata.put(wire::encode("m01-header-v1", header));
                if (device->session == header.id.session) metadata.fields(device->format, device->firmware, device->sdk);
                const auto epoch = row.input_generation.value_or(config.generation);
                const SegmentKey key{config.run_id, device->device_id, std::to_string(header.id.session),
                    "diagnostic-" + wire::key({std::to_string(epoch), wire::digest(details.data),
                                                wire::digest(metadata.data), wire::digest(packed)})};
                const auto trigger = header.trigger && header.association_verified ?
                    capture_trigger_id(*header.trigger) : std::string{};
                Block block{header.id.frame, header.id.row, {}, trigger, time(header.source_time),
                            {receipt_domain_, evidence.received.count()}, format, {},
                            wire::envelope("m01-block-v1", metadata.data)};
                const auto saved = archive_.ingest({key, epoch, format, {{std::move(block), packed}}, {}});
                if (!accepted(saved.status)) return saved;
                raw = BlockRef{key, 0};
                details.put(*raw);
            }
        }
        Record record;
        record.run = config.run_id;
        record.key = {Kind::Derived, wire::key({"m01-diagnostic", std::to_string(config.generation),
                                               wire::digest(details.data)}), 1};
        record.epoch = row.input_generation.value_or(config.generation);
        record.description = diagnostic_text(row.issue);
        record.detail = wire::envelope("m01-diagnostic-v1", details.data);
        if (raw) {
            record.raw.push_back(*raw);
            record.received = {receipt_domain_, row.evidence->received.count()};
            record.occurred = time(row.evidence->header.source_time);
        }
        rows.push_back(std::move(record));
    }
    return rows.empty() ? archive_.manifest(config.run_id) : archive_.append(config.run_id, rows);
}

Result<Manifest> M01CaptureAdapter::ingest(const m01::Output& output) {
    if (!output.config) return {Status::Invalid, {}};
    Result<Manifest> result = archive_.manifest(output.config->run_id);
    for (const auto& segment : output.segments) {
        if (!segment || segment->run_id != output.config->run_id) return {Status::Invalid, {}};
        result = ingest(*segment);
        if (!accepted(result.status)) return result;
    }
    return output.diagnostics.empty() ? result : diagnose(*output.config, output.diagnostics);
}

}  // namespace waste::m09
