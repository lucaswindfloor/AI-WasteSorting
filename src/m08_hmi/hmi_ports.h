#pragma once

#include "hmi_types.h"

namespace waste::m08 {

class IdentityPort {
public:
    virtual ~IdentityPort() = default;
    virtual std::optional<Principal> session(std::string_view credential) = 0;
    virtual std::optional<Principal> current(std::string_view subject) = 0;
};

enum class Delivery { Accepted, Rejected, Unknown };
enum class EffectState { Unknown, Pending, Applied, Rejected };
struct Effect {
    CommandKey key;
    EffectState state{EffectState::Unknown};
    Stamp stamp;
    std::string evidence;
};

class WorkCellPort {
public:
    virtual ~WorkCellPort() = default;
    virtual std::optional<Runtime> read(const Scope& scope) = 0;
    // The adapter atomically checks the declared scope, current guard
    // revision, epoch and resource-specific bindings, including the fence.
    // Accepted only confirms delivery. Commands are idempotent by key.
    virtual Delivery submit(const Command& command) = 0;
    virtual Effect lookup(const CommandKey& original) = 0;
};

enum class LoadStatus { Empty, Restored, Unavailable };
struct Load {
    LoadStatus status{LoadStatus::Unavailable};
    std::optional<Journal> journal;
};

struct Confirmation {
    bool confirmed{};
    std::uint64_t revision{};
    std::string transaction;
};

class JournalPort {
public:
    virtual ~JournalPort() = default;
    virtual Load load(const Scope& scope) = 0;
    // A matching confirmation requires an atomic, recoverable checkpoint.
    virtual Confirmation commit(std::uint64_t expected, const Journal& next) = 0;
};

struct EvidenceQuery {
    Scope scope;
    std::string manifest, cursor;
    std::size_t limit{};
};

struct EvidenceRow {
    std::string target, planned_bin, actual_bin, evidence;
};

struct EvidencePage {
    Scope scope;
    std::string manifest, next;
    std::vector<EvidenceRow> rows;
    std::vector<std::string> gaps;
};

struct ExportRequest {
    Scope scope;
    std::string id, manifest, first, last, purpose;
    std::size_t max_rows{};
    bool operator==(const ExportRequest&) const = default;
};

enum class ExportStage { Queued, Ready, Failed };
struct ExportJob {
    std::string owner;
    ExportRequest request;
    ExportStage stage{};
    std::string actual_range, checksum;
    std::vector<std::string> gaps;
};

class EvidencePort {
public:
    virtual ~EvidencePort() = default;
    // The archive checks that the authenticated reader may access the manifest.
    virtual std::optional<EvidencePage> list(const EvidenceQuery& query, std::string_view reader) = 0;
    // Idempotent on authenticated owner, scope and request ID. Changing the
    // requested range or manifest for that key returns a conflict.
    virtual std::optional<ExportJob> enqueue(const ExportRequest& request, std::string_view owner) = 0;
    virtual std::optional<ExportJob> find(const Scope& scope, std::string_view id,
                                          std::string_view owner) = 0;
    virtual std::optional<std::string> download(const ExportJob& job) = 0;
};

}  // namespace waste::m08
