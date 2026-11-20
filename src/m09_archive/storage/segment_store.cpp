#include "segment_store.h"
#include "record_codec.h"

#include <memory>
#include <openssl/evp.h>
#include <set>

namespace waste::m09 {
namespace {

constexpr std::string_view schema = "segment-v1";

std::optional<Segment> describe(const SegmentInput& input, std::uint64_t limit) {
    const auto& [run, source, session, id] = input.key;
    if (!wire::identifier(run) || !wire::identifier(source) || !wire::identifier(session) ||
        !wire::identifier(id) || !input.epoch || !wire::identifier(input.format) || input.blocks.empty() ||
        input.blocks.size() > 65536 || input.gaps.size() > 65536) return {};
    Segment result{input.key, input.epoch, input.format, {}, 0, {}, input.gaps};
    std::set<std::pair<std::uint64_t, std::uint64_t>> positions;
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) return {};
    for (const auto& item : input.blocks) {
        auto block = item.metadata;
        if (item.bytes.empty() || item.bytes.size() > limit - result.length ||
            block.metadata.size() > 64 * 1024 || !positions.emplace(block.frame, block.row).second) return {};
        block.bytes = {result.length, result.length + item.bytes.size()};
        block.digest = wire::digest(item.bytes);
        if (EVP_DigestUpdate(context.get(), item.bytes.data(), item.bytes.size()) != 1) return {};
        result.length = block.bytes.end;
        result.blocks.push_back(std::move(block));
    }
    for (const auto& gap : input.gaps) {
        if (!wire::identifier(gap.source) || !wire::identifier(gap.session) || !wire::identifier(gap.basis) ||
            !wire::identifier(gap.reason) || gap.range.begin >= gap.range.end) return {};
    }
    unsigned char bytes[EVP_MAX_MD_SIZE]{};
    unsigned int length{};
    if (EVP_DigestFinal_ex(context.get(), bytes, &length) != 1 || length != 32) return {};
    constexpr std::string_view alphabet = "0123456789abcdef";
    for (unsigned i = 0; i < length; ++i) {
        result.digest += alphabet[bytes[i] >> 4];
        result.digest += alphabet[bytes[i] & 15];
    }
    return result;
}

}  // namespace

disk::Path SegmentStore::directory(const SegmentKey& key) const {
    return path() / wire::key({key.run, key.source, key.session, key.id});
}

Result<SegmentEntry> SegmentStore::put(const SegmentInput& input, bool committed) {
    auto description = describe(input, max_bytes_);
    if (!description) return {Status::Invalid, {}};
    const auto dir = directory(input.key);
    if (!disk::ensure(dir)) return {};
    const auto metadata_file = dir / "segment";
    const auto existing = disk::read(metadata_file);
    if (existing.value) {
        auto original = wire::decode<Segment>(schema, *existing.value);
        if (!original) return {Status::Corrupt, {}};
        if (*original != *description) return {Status::Conflict, {}};
        const auto validated = inspect(dir);
        if (validated.status == Status::Ok) {
            if (!disk::sync(dir)) return {};
            auto result = validated;
            result.status = Status::Duplicate;
            return result;
        }
        if (committed) return {Status::Corrupt, {}};
        // An incomplete initial write can be retried with identical declared bytes.
    } else if (existing.status != Status::Missing) {
        return {existing.status, {}};
    }
    if (!existing.value && committed) return {Status::Corrupt, {}};
    if (!existing.value) {
        const auto metadata = wire::encode(schema, *description);
        if (metadata.size() > disk::metadata_limit) return {Status::Capacity, {}};
        if (!disk::publish(metadata_file, metadata)) return {};
    }
    if (!disk::publish(dir / "payload", [&](const Sink& write) {
        for (const auto& block : input.blocks) if (!write(block.bytes)) return false;
        return true;
    })) return {};
    return {Status::Ok, SegmentEntry{std::move(*description), std::vector<bool>(input.blocks.size(), true), {}}};
}

Result<SegmentEntry> SegmentStore::inspect(const disk::Path& dir) const {
    const auto metadata = disk::read(dir / "segment");
    if (!metadata.value) return {metadata.status, {}};
    auto segment = wire::decode<Segment>(schema, *metadata.value);
    if (!segment || segment->length > max_bytes_ || segment->blocks.empty() ||
        directory(segment->key) != dir) return {Status::Corrupt, {}};
    SegmentEntry out{std::move(*segment), {}, {}};
    // A pending file may contain a verifiable prefix. Sync and promote the
    // bytes before they can be referenced by a recovery manifest.
    const auto main = dir / "payload";
    if (!disk::size(main) && disk::size(dir / "payload.pending")) {
        // Copy to a separate staging name so the pending source survives an interruption.
        const auto recovery = dir / "recovered";
        if (!disk::publish(recovery, [&](const Sink& write) { return disk::stream(dir / "payload.pending", write); })) {
            return {};
        }
        std::error_code error;
        std::filesystem::rename(recovery, main, error);
        if (error || !disk::sync(dir)) return {};
    }
    for (const auto& block : out.segment.blocks) {
        const auto value = disk::slice(main, block.bytes, max_bytes_);
        const bool valid = value.value && wire::digest(*value.value) == block.digest;
        out.readable.push_back(valid);
        if (!valid) {
            out.recovery_gaps.push_back({out.segment.key.source, out.segment.key.session, "byte",
                                         block.bytes, "missing or damaged archived block"});
        }
    }
    if (out.recovery_gaps.empty()) {
        const auto length = disk::size(main);
        const auto digest = disk::digest(main);
        if (!length || *length != out.segment.length || !digest ||
            (!out.segment.digest.empty() && out.segment.digest != *digest)) return {Status::Corrupt, {}};
        out.segment.digest = *digest;
        return {Status::Ok, std::move(out)};
    }
    return {Status::Partial, std::move(out)};
}

Result<Bytes> SegmentStore::read(const SegmentEntry& entry, std::uint64_t block) const {
    if (block >= entry.segment.blocks.size() || block >= entry.readable.size()) return {Status::Invalid, {}};
    if (!entry.readable[block]) return {Status::Missing, {}};
    const auto& description = entry.segment.blocks[block];
    auto result = disk::slice(directory(entry.segment.key) / "payload", description.bytes, max_bytes_);
    if (!result.value || wire::digest(*result.value) != description.digest) return {Status::Corrupt, {}};
    return result;
}

}  // namespace waste::m09
