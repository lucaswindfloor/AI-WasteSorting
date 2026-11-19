#include "acquisition_service.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace waste::m01;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Config settings() {
    Config config;
    config.run_id = "P0";
    config.generation = 1;
    config.encoder_epoch = 7;
    config.sync = {"trigger", 11, "mock", "", {}, true};
    config.rgb = {"rgb", 12, "mock", "", {"RGB8", "packed", 1, 4, {}, {}, {}}, true};
    config.hsi = {"hsi", 13, "mock", "", {"U8", "packed", 1, 4, {"450", "500"}, {}, {}}, true};
    config.match_timeout = 5ms;
    config.segment_timeout = 4ms;
    config.source_stale_timeout = 30ms;
    config.max_pending = 8;
    config.recent_history = 32;
    config.max_segment_items = 1;
    config.max_inflight_segments = 8;
    config.max_segment_bytes = 16;
    config.max_packet_bytes = 8;
    config.max_packet_items = 4;
    config.buffer_capacity = 32;
    config.buffer_high_water = 24;
    config.contiguous_trigger_ids = true;
    return config;
}

TriggerKey key(std::uint64_t id) {
    return {11, 7, id};
}

Output fire(AcquisitionService& service, std::uint64_t id, Tick now,
            std::int64_t count = 33815) {
    return service.ingest(SyncInput{1, {key(id), count, 1, SourceTime{"sync", 100}}, now}, now);
}

ObservationHeader line(const SourceConfig& device, Source source, std::uint64_t frame,
                       std::optional<TriggerKey> trigger, bool verified = true) {
    return {{source, device.session, frame, 0}, trigger, verified, SourceTime{"device", 200},
            "hardware", {}, {}, Quality::Valid, {0, 2}, {}, {}};
}

Output send(AcquisitionService& service, const Config& config, const ObservationHeader& header,
            Tick now, std::byte first = std::byte{0}, std::optional<Tick> received = {}) {
    const auto* device = source_config(config, header.id.source);
    const std::array bytes{first, std::byte{1}};
    return service.ingest(PacketView{config.generation, header.id.source, header.id.session,
                                     device->format, bytes, std::span(&header, 1), received}, now);
}

const TriggerRecord& record(const Output& out) {
    check(out.segments.size() == 1 && out.segments.front()->items.size() == 1, "expected one record");
    return std::get<TriggerRecord>(out.segments.front()->items.front());
}

void pairs_by_evidence() {
    auto config = settings();
    AcquisitionService service;
    service.start(config, 0ms);
    check(!service.health().ready, "readiness before a real pair");
    auto hsi = line(config.hsi, Source::Hsi, 467, key(2147));
    check(send(service, config, hsi, 1ms).segments.empty(), "camera-first row escaped");
    fire(service, 2147, 2ms);
    const auto rgb = line(config.rgb, Source::Rgb, 7872, key(2147));
    const auto out = send(service, config, rgb, 3ms, std::byte{0}, 2500us);
    const auto& first = record(out);
    check(first.rgb->header.id.frame == 7872 && first.hsi->header.id.frame == 467 &&
              first.event.encoder_count == 33815 && first.rgb->received == 2500us &&
              first.received == 2ms && service.health().ready, "wrong provenance or readiness");

    fire(service, 2148, 4ms);
    send(service, config, line(config.rgb, Source::Rgb, 7873, key(2148)), 5ms);
    fire(service, 2149, 6ms);
    send(service, config, line(config.hsi, Source::Hsi, 468, key(2149)), 7ms);
    const auto next = send(service, config, line(config.rgb, Source::Rgb, 7874, key(2149)), 8ms);
    check(record(next).event.key.id == 2149 && record(next).hsi->header.id.frame == 468,
          "pairing shifted after a gap");

    const auto late = send(service, config, line(config.hsi, Source::Hsi, 999, key(2148)), 9ms);
    check(record(late).event.key.id == 2148 && record(late).rgb &&
              record(late).hsi_quality == Quality::Missing &&
              late.diagnostics.size() == 1 && late.diagnostics.front().issue == Issue::Late &&
              late.diagnostics.front().evidence, "late data replaced an online gap");
}

void diagnoses_sync_gaps_and_conflicts() {
    auto config = settings();
    config.max_segment_items = 4;
    AcquisitionService service;
    service.start(config, 0ms);
    fire(service, 10, 0ms);
    const auto rgb = line(config.rgb, Source::Rgb, 1, key(10));
    send(service, config, rgb, 0ms);
    check(send(service, config, rgb, 0ms).diagnostics.empty(), "same row is not idempotent");
    send(service, config, line(config.hsi, Source::Hsi, 1, key(10)), 1ms);
    const auto conflict = send(service, config, rgb, 2ms, std::byte{9});
    check(conflict.diagnostics.size() == 1 && conflict.diagnostics[0].issue == Issue::DuplicateConflict,
          "content conflict lost");
    const auto suspect = send(service, config, line(config.rgb, Source::Rgb, 2, key(11)), 2ms);
    check(suspect.diagnostics.empty(), "camera-first candidate prematurely finalized");
    const auto gaps = fire(service, 13, 3ms);
    check(gaps.diagnostics.size() == 2 && gaps.diagnostics[0].issue == Issue::MissingSync &&
              gaps.diagnostics[0].first_trigger->id == 11 &&
              gaps.diagnostics[0].last_trigger->id == 12 &&
              gaps.diagnostics[1].evidence, "missing sync range or raw row lost");
    const auto flushed = service.advance(5ms);
    check(record(flushed).rgb_quality == Quality::Unknown, "staged conflict was not applied");
    const auto newer = service.close(6ms);
    check(record(newer).event.key.id == 13, "unfinished tail not emitted");
}

void validates_batch_and_sessions() {
    auto config = settings();
    AcquisitionService service;
    service.start(config, 0ms);
    fire(service, 21, 0ms);
    fire(service, 22, 0ms);
    std::array headers{line(config.rgb, Source::Rgb, 1, key(21)),
                       line(config.rgb, Source::Rgb, 1, key(22))};
    headers[1].id.row = 1;
    headers[1].bytes = {2, 2};
    std::array<std::byte, 4> bytes{};
    auto out = service.ingest(PacketView{1, Source::Rgb, 12, config.rgb.format, bytes, headers, {}}, 1ms);
    check(out.diagnostics.empty() && service.health().payload_bytes == 4, "batch copied per row");
    bytes.fill(std::byte{9});
    auto one = send(service, config, line(config.hsi, Source::Hsi, 1, key(21)), 2ms);
    auto two = send(service, config, line(config.hsi, Source::Hsi, 2, key(22)), 2ms);
    check(record(one).rgb->payload == record(two).rgb->payload &&
              record(two).rgb->header.bytes.offset == 2 &&
              record(two).rgb->payload->at(2) == std::byte{0}, "SDK buffer ownership leaked");

    service.close(3ms);
    auto next = config;
    next.generation = 2;
    next.rgb.session = 14;
    next.rgb.format.encoding = "RGB12";
    service.start(next, 3ms);
    check(send(service, config, headers[0], 3ms).diagnostics.front().issue == Issue::OldGeneration,
          "old callback crossed the generation boundary");
    const auto stale = send(service, next, headers[0], 3ms);
    check(stale.diagnostics.front().issue == Issue::WrongSession, "old device session was reused");
    auto malformed = line(next.rgb, Source::Rgb, 1, key(21));
    malformed.bytes = {1, 2};
    const auto invalid = send(service, next, malformed, 3ms);
    check(invalid.diagnostics.front().issue == Issue::InvalidPacket, "out-of-bounds row was accepted");
    check(one.segments.front()->config->rgb.format.encoding == "RGB8", "old format was overwritten");
}

void preserves_e05_and_independent_rows() {
    auto config = settings();
    config.routing = Routing::Independent;
    config.e05 = SourceConfig{"depth", 14, "mock", "",
                              {"HEIGHT_U8", "packed", 1, 1, {}, "device", "mm"}, true};
    AcquisitionService service;
    service.start(config, 0ms);
    const auto height = line(*config.e05, Source::E05, 1, {}, false);
    const auto out = send(service, config, height, 0ms);
    const auto& obs = std::get<Observation>(out.segments[0]->items[0]);
    check(!obs.header.trigger && obs.header.source_quality == Quality::Valid &&
              obs.payload->at(0) == std::byte{0}, "zero height was treated as missing");
    auto missing = height;
    missing.id.frame = 2;
    missing.source_quality = Quality::Missing;
    const auto unavailable = send(service, config, missing, 0ms);
    check(std::get<Observation>(unavailable.segments[0]->items[0]).header.source_quality ==
              Quality::Missing, "E05 missing marker lost");
    const auto local_trigger = TriggerKey{120, 8, 1};
    const auto independent = send(service, config,
                                  line(config.rgb, Source::Rgb, 1, local_trigger), 1ms);
    check(std::get<Observation>(independent.segments[0]->items[0]).header.trigger == local_trigger,
          "independent local trigger was lost");
    send(service, config, line(config.hsi, Source::Hsi, 1, {}, false), 1ms);
    check(service.health().ready, "independent devices required a common trigger");
}

void applies_backpressure_and_releases_references() {
    auto config = settings();
    config.max_inflight_segments = 1;
    config.buffer_capacity = config.buffer_high_water = config.max_packet_bytes = 4;
    AcquisitionService service;
    service.start(config, 0ms);
    fire(service, 1, 0ms);
    send(service, config, line(config.rgb, Source::Rgb, 1, key(1)), 0ms);
    auto pair = send(service, config, line(config.hsi, Source::Hsi, 1, key(1)), 1ms);
    auto online = pair.segments.front();
    auto archive = online;
    pair = {};
    online.reset();
    check(service.health().payload_bytes == 4 && service.health().high_water,
          "online release incorrectly released archive storage");
    fire(service, 2, 2ms);
    const auto rejected = send(service, config, line(config.rgb, Source::Rgb, 2, key(2)), 2ms);
    check(rejected.diagnostics[0].issue == Issue::Capacity &&
              rejected.diagnostics[0].observation->frame == 2, "overflow lacked scope");
    const auto tail = service.close(3ms);
    check(tail.diagnostics.size() == 1 && tail.diagnostics[0].issue == Issue::ConsumerBacklog,
          "metadata delivery backlog was not reported");
    archive.reset();
    check(service.health().payload_bytes == 0 && service.health().inflight_segments == 0,
          "buffer not released after both consumers");
}

void preserves_uncertain_and_recovers_mapping() {
    auto config = settings();
    AcquisitionService service;
    service.start(config, 0ms);
    fire(service, 1, 0ms);
    auto hint = line(config.rgb, Source::Rgb, 1, key(1), false);
    const auto unknown = send(service, config, hint, 1ms);
    check(unknown.diagnostics[0].issue == Issue::UnknownAssociation &&
              unknown.diagnostics[0].evidence, "unknown trigger candidate lost its evidence");
    const auto closed = service.advance(5ms);
    check(record(closed).rgb_quality == Quality::Unknown &&
              record(closed).hsi_quality == Quality::Missing, "unknown became confirmed missing");
    fire(service, 2, 6ms);
    hint.id.frame = 2;
    hint.trigger = key(2);
    send(service, config, hint, 7ms);
    hint.association_verified = true;
    send(service, config, hint, 8ms);
    const auto repaired = send(service, config, line(config.hsi, Source::Hsi, 2, key(2)), 9ms);
    check(record(repaired).rgb_quality == Quality::Valid, "verified mapping could not update a hint");
}

void checks_source_freshness_and_trigger_boundaries() {
    auto config = settings();
    config.first_expected_trigger = 100;
    AcquisitionService service;
    service.start(config, 0ms);
    const auto first = fire(service, 102, 1ms);
    check(first.diagnostics.size() == 1 && first.diagnostics[0].first_trigger->id == 100 &&
              first.diagnostics[0].last_trigger->id == 101, "missing initial sync range");
    send(service, config, line(config.rgb, Source::Rgb, 1, key(102)), 2ms);
    send(service, config, line(config.hsi, Source::Hsi, 1, key(102)), 3ms);
    check(service.health().ready && service.health().last_valid_pair == 1ms, "freshness not reported");
    service.advance(31ms);
    check(!service.health().ready, "silence did not invalidate readiness");
    const auto tail = service.close(32ms, key(104));
    check(tail.diagnostics.size() == 1 && tail.diagnostics[0].first_trigger->id == 103 &&
              tail.diagnostics[0].last_trigger->id == 104, "missing final sync range");
}

}  // namespace

int main() {
    try {
        pairs_by_evidence();
        diagnoses_sync_gaps_and_conflicts();
        validates_batch_and_sessions();
        preserves_e05_and_independent_rows();
        applies_backpressure_and_releases_references();
        preserves_uncertain_and_recovers_mapping();
        checks_source_freshness_and_trigger_boundaries();
        std::cout << "M01: 7 scenarios passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
