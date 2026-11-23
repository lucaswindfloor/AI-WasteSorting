#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace waste::m01 {

using Tick = std::chrono::nanoseconds;

enum class Source { Sync, Rgb, Hsi, E05 };
enum class Quality { Valid, Missing, Invalid, Stale, Unknown };
enum class Routing { SharedTrigger, Independent, RgbTriggered };

constexpr bool requires_trigger(Routing routing, Source source) {
    return source == Source::Rgb ? routing != Routing::Independent :
           source == Source::Hsi && routing == Routing::SharedTrigger;
}

struct SourceTime {
    std::string clock_domain;
    std::int64_t ticks{};
    auto operator<=>(const SourceTime&) const = default;
};

struct TriggerKey {
    std::uint64_t sync_session{};
    std::uint64_t encoder_epoch{};
    std::uint64_t id{};
    auto operator<=>(const TriggerKey&) const = default;
};

struct TriggerEvent {
    TriggerKey key;
    std::int64_t encoder_count{};
    int direction{};
    std::optional<SourceTime> source_time;
    auto operator<=>(const TriggerEvent&) const = default;
};

struct ObservationId {
    Source source;
    std::uint64_t session{};
    std::uint64_t frame{};
    std::uint32_t row{};
    auto operator<=>(const ObservationId&) const = default;
};

struct DataFormat {
    std::string encoding;
    std::string transport_layout;
    std::uint32_t width{};
    std::uint32_t rows{};
    std::vector<std::string> bands;
    std::string coordinates;
    std::string units;
    auto operator<=>(const DataFormat&) const = default;
};

struct ByteRange {
    std::size_t offset{};
    std::size_t size{};
    auto operator<=>(const ByteRange&) const = default;
};

using Payload = std::shared_ptr<const std::vector<std::byte>>;

struct ObservationHeader {
    ObservationId id;
    std::optional<TriggerKey> trigger;
    // A candidate trigger can localize uncertainty without proving association.
    bool association_verified{};
    std::optional<SourceTime> source_time;
    std::string time_basis;
    std::optional<std::int64_t> encoder_count;
    std::optional<std::uint64_t> encoder_epoch;
    Quality source_quality{Quality::Valid};
    ByteRange bytes;
    std::optional<ByteRange> valid_mask;
    std::string source_status;
    auto operator<=>(const ObservationHeader&) const = default;
};

struct Observation {
    ObservationHeader header;
    Tick received{};
    Payload payload;
};

struct SourceConfig {
    std::string device_id;
    std::uint64_t session{};
    std::string firmware;
    std::string sdk;
    DataFormat format;
    // Filled after reading back and comparing the installed settings.
    bool settings_verified{};
};

struct Config {
    std::string run_id;
    std::uint64_t generation{};
    SourceConfig sync, rgb, hsi;
    std::optional<SourceConfig> e05;
    std::uint64_t encoder_epoch{};
    Routing routing{Routing::SharedTrigger};
    bool contiguous_trigger_ids{};
    std::optional<std::uint64_t> first_expected_trigger;
    Tick match_timeout{};
    Tick segment_timeout{};
    Tick source_stale_timeout{};
    std::size_t max_pending{};
    std::size_t recent_history{};
    std::size_t max_segment_items{};
    std::size_t max_inflight_segments{};
    std::size_t max_segment_bytes{};
    std::size_t max_packet_bytes{};
    std::size_t max_packet_items{};
    std::size_t buffer_capacity{};
    std::size_t buffer_high_water{};
};

struct TriggerRecord {
    TriggerEvent event;
    Tick received{};
    std::optional<Observation> rgb, hsi;
    Quality rgb_quality{Quality::Missing};
    Quality hsi_quality{Quality::Missing};
};

enum class Issue {
    OldGeneration, WrongSession, InvalidPacket, UnknownAssociation, MissingSync,
    Late, DuplicateConflict, Capacity, ConsumerBacklog
};

struct Diagnostic {
    Issue issue;
    Source source;
    // Inclusive trigger range when last_trigger is given.
    std::optional<TriggerKey> first_trigger;
    std::optional<TriggerKey> last_trigger;
    std::optional<ObservationId> observation;
    std::optional<Observation> evidence;
    std::optional<TriggerEvent> sync_evidence;
    std::optional<std::uint64_t> input_generation{};
};

using SegmentItem = std::variant<TriggerRecord, Observation, Diagnostic>;

struct Segment {
    std::string run_id;
    std::uint64_t generation{};
    std::uint64_t sequence{};
    std::shared_ptr<const Config> config;
    std::vector<SegmentItem> items;
};

struct Output {
    std::shared_ptr<const Config> config;
    // Give each required consumer its own reference. M09 releases after persistence.
    std::vector<std::shared_ptr<const Segment>> segments;
    std::vector<Diagnostic> diagnostics;
};

struct Health {
    bool active{};
    bool ready{};
    bool high_water{};
    std::size_t payload_bytes{};
    std::size_t inflight_segments{};
    std::size_t pending{};
    Tick oldest_pending_age{};
    Tick updated{};
    std::optional<Tick> last_sync, last_rgb, last_hsi, last_e05, last_valid_pair;
    std::uint64_t rejected_packets{};
    std::uint64_t unresolved_items{};
};

}  // namespace waste::m01
