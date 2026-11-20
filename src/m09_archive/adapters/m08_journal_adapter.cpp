#include "m08_journal_adapter.h"
#include "checkpoint_layouts.h"

namespace waste::m09 {
namespace {

constexpr std::string_view schema = "m08-journal-v1";

}  // namespace

M08JournalStore::M08JournalStore(ArchiveService& archive, m08::Scope scope)
    : archive_(archive), scope_(std::move(scope)),
      journal_(archive.root() / "critical" / "m08" / wire::key({scope_.station, scope_.run})) {
    if (!wire::identifier(scope_.station) || !wire::identifier(scope_.run)) throw std::invalid_argument("invalid M08 scope");
}

Result<m08::Journal> M08JournalStore::checkpoint(std::uint64_t revision) const {
    const auto stored = journal_.load(revision);
    if (!stored.value) return {stored.status, {}};
    auto state = wire::decode<m08::Journal>(schema, stored.value->state);
    if (!state || state->scope != scope_ || state->revision != stored.value->revision ||
        state->transaction != stored.value->transaction) return {Status::Corrupt, {}};
    return {Status::Ok, std::move(state)};
}

m08::Load M08JournalStore::load(const m08::Scope& scope) {
    if (scope != scope_) return {};
    auto value = checkpoint();
    if (!value.value) return {value.status == Status::Missing ? m08::LoadStatus::Empty : m08::LoadStatus::Unavailable, {}};
    return {m08::LoadStatus::Restored, std::move(value.value)};
}

m08::Confirmation M08JournalStore::commit(std::uint64_t expected, const m08::Journal& next) {
    if (next.scope != scope_) return {};
    const auto status = journal_.commit(expected, {next.revision, next.transaction, wire::encode(schema, next)});
    return accepted(status) ? m08::Confirmation{true, next.revision, next.transaction} : m08::Confirmation{};
}

Result<Manifest> M08JournalStore::publish(std::uint64_t revision) {
    const auto saved = checkpoint(revision);
    if (!saved.value) return {saved.status, {}};
    const auto& state = *saved.value;
    Record audit;
    audit.run = scope_.run;
    audit.key = {Kind::Audit, wire::key({"m08", scope_.station}), state.revision};
    audit.description = "M08 operation and audit checkpoint";
    audit.detail = wire::encode(schema, state);
    std::vector<Record> rows;
    for (const auto& [epoch, configuration] : state.activated) {
        Record record;
        record.run = scope_.run;
        record.key = {Kind::Configuration, wire::key({"m08", scope_.station, configuration.package}), epoch};
        record.epoch = epoch;
        record.description = configuration.package;
        record.detail = wire::encode("m08-config-v1", configuration);
        audit.related.push_back(record.key);
        rows.push_back(std::move(record));
    }
    rows.push_back(std::move(audit));
    return archive_.append(scope_.run, rows);
}

}  // namespace waste::m09
