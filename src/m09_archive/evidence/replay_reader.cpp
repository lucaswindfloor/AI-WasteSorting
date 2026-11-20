#include "replay_reader.h"
#include "../storage/record_codec.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <tuple>

namespace waste::m09 {
namespace {

std::optional<Bytes> read_frame(std::istream& input, std::uint64_t limit) {
    std::array<std::byte, 8> length{};
    if (!input.read(reinterpret_cast<char*>(length.data()), length.size())) return {};
    wire::Decoder decode(length);
    std::uint64_t size{};
    decode.get(size);
    if (size > limit) return {};
    Bytes data(static_cast<std::size_t>(size));
    if (size && !input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) return {};
    return data;
}

}  // namespace

Result<PackageContents> read_evidence_package(const disk::Path& file, std::string_view digest,
                                             const ReplayRecordSink& records,
                                             const ReplayBlockSink& blocks) {
    if (digest.size() != 64 || disk::digest(file) != digest) return {Status::Corrupt, {}};
    std::ifstream input(file, std::ios::binary);
    constexpr std::string_view magic = "M09EVIDENCE1\n";
    std::string signature(magic.size(), '\0');
    if (!input.read(signature.data(), signature.size()) || signature != magic) return {Status::Corrupt, {}};
    const auto summary = read_frame(input, disk::metadata_limit);
    const auto authority = read_frame(input, disk::metadata_limit);
    auto manifest = summary ? wire::decode<Manifest>("export-manifest-v1", *summary) : std::nullopt;
    auto approved = authority ? wire::decode<Selection>("export-approved-selection-v1", *authority) : std::nullopt;
    if (!manifest || !approved || !valid_selection(*approved) || !manifest->version ||
        !wire::identifier(manifest->run)) return {Status::Corrupt, {}};

    for (const auto& key : manifest->records) {
        const auto data = read_frame(input, disk::metadata_limit);
        const auto row = data ? wire::decode<Record>("export-record-v1", *data) : std::nullopt;
        if (!row || row->run != manifest->run || row->key != key) return {Status::Corrupt, {}};
        if (records && !records(*row)) return {Status::Unavailable, {}};
    }
    for (const auto& segment : manifest->segments) {
        if (!approved->include_raw || !includes_source(*approved, segment.segment.key.source) ||
            segment.segment.key.run != manifest->run || segment.readable.size() != segment.segment.blocks.size()) {
            return {Status::Corrupt, {}};
        }
        for (std::size_t i = 0; i < segment.readable.size(); ++i) {
            if (!segment.readable[i]) continue;
            const auto& block = segment.segment.blocks[i];
            const auto data = read_frame(input, disk::metadata_limit);
            if (!data || block.bytes.begin > block.bytes.end || data->size() != block.bytes.end - block.bytes.begin ||
                wire::digest(*data) != block.digest) return {Status::Corrupt, {}};
            if (blocks && !blocks({segment.segment.key, i}, *data)) return {Status::Unavailable, {}};
        }
    }
    if (input.peek() != std::char_traits<char>::eof() || input.bad()) return {Status::Corrupt, {}};
    return {Status::Ok, PackageContents{std::move(*manifest), std::move(*approved)}};
}

Result<ReplayPlan> ReplayReader::plan(std::string_view subject, std::string_view run, std::uint64_t version,
                                     ReplayMode mode, const Selection& selection) const {
    if (!version || !valid_selection(selection) ||
        (mode != ReplayMode::AsObserved && mode != ReplayMode::WithSupplements)) return {Status::Invalid, {}};
    const auto approved = authorize_ ? authorize_(subject, run, Access::Read, "offline replay", selection)
                                     : std::nullopt;
    if (!approved || !subset(*approved, selection)) return {Status::Denied, {}};
    const auto manifest = archive_.manifest(run, version);
    if (!manifest.value) return {manifest.status, {}};
    ReplayPlan out{std::string(run), version, mode, {}, {}, false};
    const auto include_gap = [&](const Gap& gap) {
        if (includes_source(*approved, gap.source)) out.gaps.push_back(gap);
    };
    for (const auto& gap : manifest.value->gaps) include_gap(gap);
    const auto timing = [&](const Time& occurred, const Time& received) {
        return mode == ReplayMode::WithSupplements && !occurred.domain.empty() ? occurred : received;
    };
    bool missing = false;
    const auto range = record_span(*approved, manifest.value->records.size());
    for (auto i = range.begin; i < range.end; ++i) {
        const auto& key = manifest.value->records[i];
        const auto record = archive_.record(*manifest.value, key);
        if (!record.value) {
            missing = true;
            continue;
        }
        out.items.push_back({timing(record.value->occurred, record.value->received), key, {}});
    }
    for (const auto& entry : manifest.value->segments) {
        if (!approved->include_raw || !includes_source(*approved, entry.segment.key.source)) continue;
        for (const auto& gap : entry.segment.gaps) include_gap(gap);
        for (const auto& gap : entry.recovery_gaps) include_gap(gap);
        for (std::size_t i = 0; i < entry.readable.size(); ++i) {
            if (!entry.readable[i]) continue;
            const auto& block = entry.segment.blocks[i];
            out.items.push_back({timing(block.occurred, block.received), {}, BlockRef{entry.segment.key, i}});
        }
    }
    std::set<std::string> domains;
    for (const auto& item : out.items) domains.insert(item.time.domain);
    out.cross_domain_unordered = domains.size() > 1 || domains.contains("");
    // Time order is meaningful only *within* each group. Preserve insertion
    // order for equal timestamps and expose the missing clock alignment.
    std::stable_sort(out.items.begin(), out.items.end(), [](const auto& a, const auto& b) {
        return std::tie(a.time.domain, a.time.ticks) < std::tie(b.time.domain, b.time.ticks);
    });
    return {missing || out.cross_domain_unordered ? Status::Partial : Status::Ok, std::move(out)};
}

}  // namespace waste::m09
