#include "reconciliation_service.h"
#include "../storage/record_codec.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace waste::m09 {
namespace {

constexpr std::string_view pass_schema = "material-pass-v1";
constexpr std::string_view measurement_schema = "measurement-v1";
constexpr std::string_view revision_schema = "reconciliation-v1";

bool valid_time(const Time& value) { return wire::identifier(value.domain); }

bool valid_position(const Position& point) {
    return wire::identifier(point.frame) && valid_time(point.reference) &&
           std::isfinite(point.x_mm) && std::isfinite(point.y_mm) && std::isfinite(point.z_mm);
}

bool valid_destination(Destination destination, std::string_view bin) {
    return destination >= Destination::Unknown && destination <= Destination::Other &&
           (destination == Destination::Bin ? wire::identifier(bin) : bin.empty());
}

Record make_row(std::string run, RecordKey key, Time received, std::string description) {
    Record row;
    row.run = std::move(run);
    row.key = std::move(key);
    row.received = std::move(received);
    row.description = std::move(description);
    return row;
}

std::string outcome(Destination destination, const std::string& bin) {
    switch (destination) {
    case Destination::Bin: return bin;
    case Destination::BeltTail: return "带尾";
    case Destination::Other: return "其他去向";
    default: return {};
    }
}

}  // namespace

RecordKey pass_key(std::string_view scope, std::string_view pass) {
    return {Kind::Pass, wire::key({scope, pass}), 1};
}

RecordKey measurement_key(std::string_view scope, std::string_view pass, std::string_view id) {
    return {Kind::Measurement, wire::key({scope, pass, id}), 1};
}

bool ReconciliationService::allowed(std::string_view subject, std::string_view run, std::string_view purpose) const {
    const Selection requested;
    const auto grant = authorize_ ? authorize_(subject, run, Access::Reconcile, purpose, requested) : std::nullopt;
    return wire::identifier(subject) && grant && *grant == requested;
}

Result<Manifest> ReconciliationService::register_pass(std::string_view subject, const MaterialPass& pass) {
    if (!allowed(subject, pass.run, "record independent entry")) return {Status::Denied, {}};
    if (!wire::identifier(pass.scope) || !wire::identifier(pass.id) || !wire::identifier(pass.evidence) ||
        !valid_time(pass.received)) return {Status::Invalid, {}};
    auto row = make_row(pass.run, pass_key(pass.scope, pass.id), pass.received, "Independently observed material entry");
    row.detail = wire::encode(pass_schema, pass);
    return archive_.append(pass.run, {row});
}

Result<Manifest> ReconciliationService::measure(std::string_view subject, const Measurement& value) {
    if (!allowed(subject, value.run, "record independent measurement")) return {Status::Denied, {}};
    if (!wire::identifier(value.scope) || !wire::identifier(value.pass) || !wire::identifier(value.id) ||
        !wire::identifier(value.evidence) || !valid_time(value.occurred) || !valid_time(value.received) ||
        (!value.valid && !wire::identifier(value.reason)) || !valid_destination(value.destination, value.bin)) {
        return {Status::Invalid, {}};
    }
    const auto has_pose = value.reference || value.instructed || value.actual;
    if (value.kind == MeasurementKind::Contact) {
        if (value.destination != Destination::Unknown) return {Status::Invalid, {}};
        if (has_pose && (!value.contact || !value.reference || !value.instructed || !value.actual ||
            !valid_position(*value.reference) || !valid_position(*value.instructed) ||
            !valid_position(*value.actual) || value.reference->frame != value.instructed->frame ||
            value.reference->frame != value.actual->frame || value.reference->reference != value.instructed->reference ||
            value.reference->reference != value.actual->reference)) return {Status::Invalid, {}};
    } else if (value.kind != MeasurementKind::Destination || value.contact || has_pose) {
        return {Status::Invalid, {}};
    }
    const auto snapshot = archive_.manifest(value.run);
    if (!snapshot.value) return snapshot;
    const auto pass = pass_key(value.scope, value.pass);
    if (!archive_.record(*snapshot.value, pass).value) return {Status::Missing, {}};
    if (value.attempt && (value.attempt->kind != Kind::Attempt ||
        !archive_.record(*snapshot.value, *value.attempt).value)) return {Status::Invalid, {}};
    auto row = make_row(value.run, measurement_key(value.scope, value.pass, value.id), value.received,
                        "Independent measurement");
    row.occurred = value.occurred;
    row.related.push_back(pass);
    if (value.attempt) row.related.push_back(*value.attempt);
    row.actual_bin = value.valid ? outcome(value.destination, value.bin) : "";
    row.detail = wire::encode(measurement_schema, value);
    return archive_.append(value.run, {row});
}

Result<Manifest> ReconciliationService::revise(std::string_view subject, const Reconciliation& revision) {
    if (!allowed(subject, revision.run, "revise independent reconciliation")) return {Status::Denied, {}};
    if (revision.reviewer != subject || !wire::identifier(revision.scope) || !wire::identifier(revision.pass) ||
        !wire::identifier(revision.reason) || !valid_time(revision.reviewed) || !revision.version ||
        revision.previous != revision.version - 1 || revision.match < Match::Pending ||
        revision.match > Match::VerifiedMiss || !valid_destination(revision.destination, revision.actual_bin) ||
        !std::ranges::all_of(revision.evidence, wire::identifier)) return {Status::Invalid, {}};
    std::set<std::string> candidates;
    for (const auto& target : revision.targets) candidates.insert(target.id);
    if ((revision.match == Match::VerifiedMiss && (!revision.coverage_verified || !candidates.empty() ||
          !revision.attempts.empty() || revision.evidence.empty())) ||
        (revision.match == Match::Matched && (candidates.size() != 1 || revision.evidence.empty())) ||
        (revision.match == Match::Ambiguous && candidates.size() < 2 && revision.attempts.size() < 2)) {
        return {Status::Invalid, {}};
    }
    auto snapshot = archive_.manifest(revision.run);
    if (!snapshot.value) return snapshot;
    const auto& current = *snapshot.value;
    const auto pass = pass_key(revision.scope, revision.pass);
    if (!archive_.record(current, pass).value) return {Status::Missing, {}};
    auto row = make_row(revision.run, {Kind::Reconciliation, pass.id, revision.version}, revision.reviewed, revision.reason);
    row.related.push_back(pass);
    if (revision.previous) row.related.push_back({Kind::Reconciliation, pass.id, revision.previous});
    row.detail = wire::encode(revision_schema, revision);
    if (revision.match == Match::Matched) row.target = *candidates.begin();
    row.actual_bin = outcome(revision.destination, revision.actual_bin);
    for (const auto& ref : revision.targets) {
        if (ref.kind != Kind::Target || !archive_.record(current, ref).value) return {Status::Invalid, {}};
        row.related.push_back(ref);
    }
    for (const auto& ref : revision.attempts) {
        const auto attempt = archive_.record(current, ref);
        if (ref.kind != Kind::Attempt || !attempt.value) return {Status::Invalid, {}};
        if (revision.match == Match::Matched && attempt.value->target != row.target) return {Status::Invalid, {}};
        if (row.planned_bin.empty()) row.planned_bin = attempt.value->planned_bin;
        else if (attempt.value->planned_bin != row.planned_bin) row.planned_bin = "多个计划箱";
        row.related.push_back(ref);
    }
    bool destination_evidenced = revision.destination == Destination::Unknown;
    for (const auto& ref : revision.measurements) {
        const auto observed = archive_.record(current, ref);
        if (ref.kind != Kind::Measurement || !observed.value) return {Status::Invalid, {}};
        const auto measurement = wire::decode<Measurement>(measurement_schema, observed.value->detail);
        if (!measurement || measurement->scope != revision.scope || measurement->pass != revision.pass ||
            measurement->run != revision.run) return {Status::Invalid, {}};
        if (measurement->valid && measurement->kind == MeasurementKind::Destination &&
            measurement->destination == revision.destination && measurement->bin == revision.actual_bin) {
            destination_evidenced = true;
        }
        row.related.push_back(ref);
    }
    if (!destination_evidenced) return {Status::Invalid, {}};

    const auto existing = archive_.record(current, row.key);
    if (existing.value) {
        return *existing.value == row ? Result<Manifest>{Status::Duplicate, std::move(snapshot.value)}
                                      : Result<Manifest>{Status::Conflict, {}};
    }
    std::uint64_t latest = 0;
    for (const auto& key : current.records) {
        if (key.kind == Kind::Reconciliation && key.id == pass.id) latest = std::max(latest, key.version);
    }
    if (latest != revision.previous) return {Status::Conflict, {}};
    return archive_.append(revision.run, {row}, current.version);
}

std::vector<BatchRow> batch_rows(const ArchiveService& archive, const Manifest& manifest,
                                 Range range, std::uint64_t limit) {
    std::map<std::string, RecordKey> latest;
    for (auto i = range.begin; i < range.end; ++i) {
        const auto& key = manifest.records[i];
        if (key.kind != Kind::Reconciliation) continue;
        const auto it = latest.find(key.id);
        if (it == latest.end() || it->second.version < key.version) latest[key.id] = key;
    }
    std::vector<BatchRow> result;
    std::set<RecordKey> matched_attempts;
    for (auto i = range.begin; i < range.end; ++i) {
        const auto& key = manifest.records[i];
        if (key.kind != Kind::Pass) continue;
        const auto record = archive.record(manifest, key);
        if (!record.value) continue;
        const auto pass = wire::decode<MaterialPass>(pass_schema, record.value->detail);
        if (!pass) continue;
        BatchRow row{i, pass->id, {}, {}, {}, pass->evidence};
        if (const auto found = latest.find(key.id); found != latest.end()) {
            const auto revision = archive.record(manifest, found->second);
            if (revision.value) {
                row.target = revision.value->target;
                row.planned = revision.value->planned_bin;
                row.actual = revision.value->actual_bin;
                row.evidence = revision.value->description;
                for (const auto& link : revision.value->related) {
                    if (!row.target.empty() && link.kind == Kind::Attempt) matched_attempts.insert(link);
                }
            }
        }
        result.push_back(std::move(row));
    }
    for (auto i = range.begin; i < range.end; ++i) {
        const auto& key = manifest.records[i];
        if (key.kind != Kind::Attempt || matched_attempts.contains(key)) continue;
        const auto row = archive.record(manifest, key);
        if (row.value) {
            result.push_back({i, {}, row.value->target, row.value->planned_bin, {}, "No associated independent result"});
        }
    }
    std::ranges::sort(result, {}, &BatchRow::ordinal);
    if (result.size() > limit) result.resize(static_cast<std::size_t>(limit));
    return result;
}

}  // namespace waste::m09
