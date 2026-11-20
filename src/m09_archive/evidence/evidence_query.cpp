#include "evidence_query.h"
#include "../storage/record_codec.h"

#include <algorithm>
#include <deque>
#include <set>

namespace waste::m09 {

bool valid_selection(const Selection& selection) {
    return selection.sources.size() <= 256 && std::ranges::all_of(selection.sources, wire::identifier) &&
           (!selection.record_range || selection.record_range->begin < selection.record_range->end);
}

bool subset(const Selection& approved, const Selection& wanted) {
    return valid_selection(approved) && valid_selection(wanted) && (!approved.include_raw || wanted.include_raw) &&
           (wanted.sources.empty() || (!approved.sources.empty() && std::ranges::all_of(approved.sources,
               [&](const std::string& source) { return includes_source(wanted, source); }))) &&
           (!wanted.record_range || (approved.record_range && approved.record_range->begin >= wanted.record_range->begin &&
                                      approved.record_range->end <= wanted.record_range->end));
}

bool includes_source(const Selection& selection, std::string_view source) {
    return selection.sources.empty() || std::ranges::find(selection.sources, source) != selection.sources.end();
}

Range record_span(const Selection& selection, std::uint64_t size) {
    if (!selection.record_range) return {0, size};
    return {std::min(size, selection.record_range->begin), std::min(size, selection.record_range->end)};
}

Result<Trace> EvidenceQuery::trace(std::string_view subject, std::string_view run, std::uint64_t version,
                                    const RecordKey& start, std::size_t limit) const {
    if (!valid_record_key(start)) return {Status::Invalid, {}};
    return lookup(subject, run, version, &start, {}, limit);
}

Result<Trace> EvidenceQuery::target(std::string_view subject, std::string_view run, std::uint64_t version,
                                     std::string_view id, std::size_t limit) const {
    if (!wire::identifier(id)) return {Status::Invalid, {}};
    return lookup(subject, run, version, nullptr, id, limit);
}

Result<Trace> EvidenceQuery::lookup(std::string_view subject, std::string_view run, std::uint64_t version,
                                     const RecordKey* start, std::string_view target, std::size_t limit) const {
    if (!limit || limit > 1000) return {Status::Invalid, {}};
    const Selection requested{{}, {}, true};
    const auto approval = authorize_ ? authorize_(subject, run, Access::Read, "trace", requested) : std::nullopt;
    if (!approval || !subset(*approval, requested)) return {Status::Denied, {}};
    auto snapshot = archive_.manifest(run, version);
    if (!snapshot.value) return {snapshot.status, {}};
    auto& manifest = *snapshot.value;
    const auto range = record_span(*approval, manifest.records.size());
    const auto first = manifest.records.begin() + static_cast<std::ptrdiff_t>(range.begin);
    const auto last = manifest.records.begin() + static_cast<std::ptrdiff_t>(range.end);
    manifest.records = std::vector<RecordKey>(first, last);
    EvidenceIndex index(archive_, manifest);
    Trace out;
    out.manifest = manifest.version;
    std::deque<RecordKey> queue;
    if (start) {
        if (!index.records.contains(*start)) {
            return {std::ranges::find(index.unavailable, *start) == index.unavailable.end()
                    ? Status::Missing : Status::Corrupt, {}};
        }
        queue.push_back(*start);
    } else if (const auto found = index.targets.find(std::string(target)); found != index.targets.end()) {
        queue.assign(found->second.begin(), found->second.end());
    } else {
        return {index.unavailable.empty() ? Status::Missing : Status::Partial, {}};
    }
    std::set<RecordKey> visited;
    const auto permitted_raw = [&](const BlockRef& ref) {
        return approval->include_raw && includes_source(*approval, ref.segment.source);
    };
    while (!queue.empty() && visited.size() < limit) {
        auto key = queue.front();
        queue.pop_front();
        if (!visited.insert(key).second) continue;
        const auto record = index.records.find(key);
        if (record == index.records.end()) {
            if (!approval->record_range) out.pending.push_back(std::move(key));
            continue;
        }
        auto row = record->second;
        for (const auto& link : row.related) queue.push_back(link);
        if (const auto linked = index.reverse.find(key); linked != index.reverse.end()) {
            for (const auto& link : linked->second) queue.push_back(link);
        }
        if (approval->record_range) {
            std::erase_if(row.related, [&](const auto& link) { return !index.records.contains(link); });
        }
        std::erase_if(row.raw, [&](const auto& ref) { return !permitted_raw(ref); });
        if (!approval->sources.empty() || approval->record_range || !approval->include_raw) row.detail.clear();
        for (const auto& ref : row.raw) {
            if (std::ranges::find(out.raw, ref) == out.raw.end()) out.raw.push_back(ref);
        }
        out.records.push_back(std::move(row));
    }
    out.truncated = !queue.empty();
    const auto add_gap = [&](const Gap& gap) {
        if (includes_source(*approval, gap.source) && out.gaps.size() < limit) out.gaps.push_back(gap);
    };
    for (const auto& gap : manifest.gaps) add_gap(gap);
    for (const auto& row : manifest.segments) {
        for (const auto& gap : row.segment.gaps) add_gap(gap);
        for (const auto& gap : row.recovery_gaps) add_gap(gap);
    }
    const bool complete = !out.truncated && out.pending.empty() && index.unavailable.empty();
    return {complete ? Status::Ok : Status::Partial, std::move(out)};
}

Result<Bytes> EvidenceQuery::raw(std::string_view subject, std::uint64_t version, const BlockRef& ref) const {
    const Selection requested{{ref.segment.source}, {}, true};
    const auto approval = authorize_ ? authorize_(subject, ref.segment.run, Access::Read, "raw", requested)
                                     : std::nullopt;
    if (!approval || !subset(*approval, requested) || !approval->include_raw ||
        !includes_source(*approval, ref.segment.source) || approval->record_range) return {Status::Denied, {}};
    const auto manifest = archive_.manifest(ref.segment.run, version);
    return manifest.value ? archive_.raw(*manifest.value, ref) : Result<Bytes>{manifest.status, {}};
}

}  // namespace waste::m09
