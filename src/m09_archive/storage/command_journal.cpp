#include "command_journal.h"
#include "record_codec.h"

namespace waste::m09::wire {
template<> struct Layout<Checkpoint> {
    static void visit(auto& value, auto&& fields) { fields(value.revision, value.transaction, value.state, value.intent_digests); }
};
}

namespace waste::m09 {
namespace {

constexpr std::string_view schema = "checkpoint-v1";
constexpr std::string_view head_schema = "checkpoint-head-v1";

}  // namespace

disk::Path CommandJournal::checkpoint_path(std::uint64_t revision) const {
    return root_ / (std::to_string(revision) + ".checkpoint");
}

Result<std::uint64_t> CommandJournal::head() const {
    const auto bytes = disk::read(root_ / "head");
    if (!bytes.value) {
        if (bytes.status != Status::Missing) return {bytes.status, {}};
        std::error_code error;
        std::filesystem::directory_iterator entry(root_, error), end;
        for (; !error && entry != end; entry.increment(error)) {
            if (entry->path().extension() == ".checkpoint") return {Status::Corrupt, {}};
        }
        return {error ? Status::Unavailable : Status::Missing, {}};
    }
    if (!disk::sync(root_)) return {};
    auto head = wire::decode<std::uint64_t>(head_schema, *bytes.value);
    if (!head) return {Status::Corrupt, {}};
    return {Status::Ok, head};
}

Result<Checkpoint> CommandJournal::load_at(std::uint64_t revision) const {
    const auto bytes = disk::read(checkpoint_path(revision));
    if (!bytes.value) return {bytes.status == Status::Missing ? Status::Corrupt : bytes.status, {}};
    auto decoded = wire::decode<Checkpoint>(schema, *bytes.value);
    if (!decoded || decoded->revision != revision || !wire::identifier(decoded->transaction) ||
        decoded->state.empty()) return {Status::Corrupt, {}};
    return {Status::Ok, std::move(decoded)};
}

Result<Checkpoint> CommandJournal::load(std::uint64_t revision) const {
    std::error_code error;
    if (!std::filesystem::is_directory(root_, error)) {
        return {error && error != std::errc::no_such_file_or_directory ? Status::Unavailable : Status::Missing, {}};
    }
    const disk::Lock lock(root_);
    if (!lock) return {};
    const auto top = head();
    if (!top.value) return {top.status, {}};
    if (!*top.value || revision > *top.value) return {Status::Missing, {}};
    return load_at(revision ? revision : *top.value);
}

Status CommandJournal::commit(std::uint64_t expected, const Checkpoint& next) const {
    if (expected == UINT64_MAX || next.revision != expected + 1 || !wire::identifier(next.transaction) ||
        next.state.empty()) return Status::Invalid;
    if (next.state.size() > disk::metadata_limit / 2 || next.intent_digests.size() > 100000) {
        return Status::Capacity;
    }
    if (!disk::ensure(root_)) return Status::Unavailable;
    const disk::Lock lock(root_);
    if (!lock) return Status::Unavailable;
    const auto top = head();
    if (!top.value && top.status != Status::Missing) return top.status;
    const auto revision = top.value.value_or(0);
    if (revision == next.revision) {
        const auto existing = load_at(revision);
        if (!existing.value) return existing.status;
        if (existing.value->state != next.state || existing.value->transaction != next.transaction) {
            return Status::Conflict;
        }
        for (const auto& [key, digest] : next.intent_digests) {
            const auto original = existing.value->intent_digests.find(key);
            if (original == existing.value->intent_digests.end() || original->second != digest) return Status::Conflict;
        }
        return Status::Duplicate;
    }
    if (revision != expected) return Status::Conflict;
    if (!top.value && !disk::publish(root_ / "head", wire::encode(head_schema, std::uint64_t{0}))) {
        return Status::Unavailable;
    }
    auto persisted = next;
    if (revision) {
        const auto previous = load_at(revision);
        if (!previous.value) return previous.status;
        for (const auto& [key, digest] : previous.value->intent_digests) {
            const auto [entry, inserted] = persisted.intent_digests.try_emplace(key, digest);
            if (!inserted && entry->second != digest) return Status::Conflict;
        }
    }
    if (persisted.intent_digests.size() > 100000) return Status::Capacity;
    const auto stored = wire::encode(schema, persisted);
    if (stored.size() > disk::metadata_limit) return Status::Capacity;
    // A revision above the verified head can only be an unconfirmed orphan.
    // Publishing the checksum-protected head makes the exact revision visible.
    if (!disk::publish(checkpoint_path(next.revision), stored) ||
        !disk::publish(root_ / "head", wire::encode(head_schema, next.revision))) {
        return Status::Unavailable;
    }
    return Status::Ok;
}

}  // namespace waste::m09
