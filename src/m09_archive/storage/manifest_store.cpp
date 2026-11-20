#include "manifest_store.h"
#include "record_codec.h"

#include <algorithm>
#include <charconv>

namespace waste::m09 {
namespace {

constexpr std::string_view schema = "manifest-v1";

}  // namespace

disk::Path ManifestStore::file(std::uint64_t version) const {
    return root_ / "manifests" / (std::to_string(version) + ".manifest");
}

Result<std::vector<std::uint64_t>> ManifestStore::versions() const {
    std::vector<std::uint64_t> versions;
    std::error_code error;
    std::filesystem::directory_iterator i(root_ / "manifests", error), end;
    if (error == std::errc::no_such_file_or_directory) return {Status::Ok, versions};
    for (; !error && i != end; i.increment(error)) {
        if (i->path().extension() != ".manifest") continue;
        const auto name = i->path().stem().string();
        std::uint64_t version{};
        const auto [last, result] = std::from_chars(name.data(), name.data() + name.size(), version);
        if (last != name.data() + name.size() || result != std::errc{} ||
            !version || name != std::to_string(version)) return {Status::Corrupt, {}};
        versions.push_back(version);
    }
    if (error) return {};
    std::ranges::sort(versions);
    return {Status::Ok, std::move(versions)};
}

Result<Manifest> ManifestStore::load(std::uint64_t version) const {
    if (!version) {
        const auto listed = versions();
        if (!listed.value) return {listed.status, {}};
        if (listed.value->empty()) return {Status::Missing, {}};
        version = listed.value->back();
    }
    const auto location = file(version);
    const auto bytes = disk::read(location);
    if (!bytes.value) return {bytes.status, {}};
    if (!disk::sync(location.parent_path())) return {};
    auto manifest = wire::decode<Manifest>(schema, *bytes.value);
    if (!manifest || manifest->version != version || manifest->run != run_ ||
        manifest->previous >= version) return {Status::Corrupt, {}};
    return {Status::Ok, std::move(manifest)};
}

Status ManifestStore::commit(const Manifest& manifest) const {
    if (manifest.run != run_ || !manifest.version || manifest.previous >= manifest.version) return Status::Invalid;
    const auto location = file(manifest.version);
    if (!disk::ensure(location.parent_path())) return Status::Unavailable;
    const auto old = disk::read(location);
    if (old.status != Status::Missing) return old.value ? Status::Conflict : old.status;
    const auto encoded = wire::encode(schema, manifest);
    if (encoded.size() > disk::metadata_limit) return Status::Capacity;
    return disk::publish(location, encoded) ? Status::Ok : Status::Unavailable;
}

}  // namespace waste::m09
