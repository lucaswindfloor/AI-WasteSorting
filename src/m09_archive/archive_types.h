#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace waste::m09 {

using Bytes = std::vector<std::byte>;
using Tick = std::chrono::nanoseconds;

enum class Status { Ok, Duplicate, Invalid, Conflict, Unavailable, Corrupt, Missing, Denied,
                    Capacity, Pending, Partial };

constexpr bool accepted(Status result) { return result == Status::Ok || result == Status::Duplicate; }

template<class T>
struct Result {
    Status status{Status::Unavailable};
    std::optional<T> value;
};

struct Time {
    std::string domain;
    std::int64_t ticks{};
    bool operator==(const Time&) const = default;
};

struct Range {
    std::uint64_t begin{}, end{};
    auto operator<=>(const Range&) const = default;
};

struct SegmentKey {
    std::string run, source, session, id;
    auto operator<=>(const SegmentKey&) const = default;
};

struct Gap {
    std::string source, session, basis;
    Range range;
    std::string reason;
    bool operator==(const Gap&) const = default;
};

struct Block {
    // Native row indices can reset on every frame. Retain both components.
    std::uint64_t frame{}, row{};
    Range bytes;
    std::string trigger;
    Time occurred, received;
    std::string format, digest;
    // Versioned source metadata, e.g. pixel layout or validity-mask location.
    Bytes metadata;
    bool operator==(const Block&) const = default;
};

struct Segment {
    SegmentKey key;
    std::uint64_t epoch{};
    std::string format, digest;
    std::uint64_t length{};
    std::vector<Block> blocks;
    std::vector<Gap> gaps;
    bool operator==(const Segment&) const = default;
};

struct SegmentEntry {
    Segment segment;
    std::vector<bool> readable;
    std::vector<Gap> recovery_gaps;
    bool operator==(const SegmentEntry&) const = default;
};

enum class Kind { Configuration, Trigger, Derived, Target, Attempt, Intent, Execution,
                  Snapshot, Audit, Pass, Measurement, Reconciliation };

struct RecordKey {
    Kind kind{};
    std::string id;
    std::uint64_t version{};
    auto operator<=>(const RecordKey&) const = default;
};

struct BlockRef {
    SegmentKey segment;
    std::uint64_t index{};
    bool operator==(const BlockRef&) const = default;
};

struct Record {
    std::string run;
    RecordKey key;
    std::uint64_t epoch{};
    Time occurred, received;
    std::vector<RecordKey> related;
    std::vector<BlockRef> raw;
    std::string target, planned_bin, actual_bin, description;
    Bytes detail;
    bool operator==(const Record&) const = default;
};

struct Manifest {
    std::string run;
    std::uint64_t version{}, previous{};
    bool sealed{}, reconciliation_pending{true};
    std::string reason;
    std::vector<SegmentEntry> segments;
    std::vector<RecordKey> records;
    std::vector<Gap> gaps;
    bool operator==(const Manifest&) const = default;
};

enum class Match { Pending, Ambiguous, Matched, VerifiedMiss };
enum class Destination { Unknown, Bin, BeltTail, Other };

struct MaterialPass {
    std::string run, scope, id, sample;
    Time received;
    std::string evidence;
    bool operator==(const MaterialPass&) const = default;
};

struct Reconciliation {
    std::string run, scope, pass;
    std::uint64_t version{}, previous{};
    Match match{};
    Destination destination{};
    std::string actual_bin, reviewer, reason;
    Time reviewed;
    bool coverage_verified{};
    std::vector<RecordKey> targets, attempts, measurements;
    std::vector<std::string> evidence;
    bool operator==(const Reconciliation&) const = default;
};

enum class MeasurementKind { Contact, Destination };

struct Position {
    std::string frame;
    Time reference;
    double x_mm{}, y_mm{}, z_mm{};
    bool operator==(const Position&) const = default;
};

struct Measurement {
    std::string run, scope, pass, id;
    MeasurementKind kind{};
    Time occurred, received;
    std::optional<RecordKey> attempt;
    bool valid{}, contact{};
    std::optional<Position> reference, instructed, actual;
    Destination destination{};
    std::string bin, evidence, reason;
    bool operator==(const Measurement&) const = default;
};

enum class Access { Read, Export, Reconcile };

struct Selection {
    std::vector<std::string> sources;
    std::optional<Range> record_range;
    bool include_raw{};
    bool operator==(const Selection&) const = default;
};

enum class ExportFormat { Package, Csv };

struct ExportRequest {
    std::string run, id, owner, purpose;
    std::uint64_t manifest{};
    Selection selection;
    std::uint64_t byte_limit{};
    ExportFormat format{ExportFormat::Package};
    std::uint64_t row_limit{};
    std::string first_label, last_label;
    bool operator==(const ExportRequest&) const = default;
};

enum class ExportStage { Queued, Ready, Failed };
struct ExportJob {
    ExportRequest request;
    Selection approved;
    ExportStage stage{};
    Status result{Status::Pending};
    std::uint64_t bytes{};
    std::string digest;
    bool operator==(const ExportJob&) const = default;
};

}  // namespace waste::m09
