#pragma once

#include "../archive_ports.h"

#include <filesystem>

namespace waste::m09::disk {

using Path = std::filesystem::path;
using Producer = std::function<bool(const Sink&)>;
constexpr std::uint64_t metadata_limit = 64 * 1024 * 1024;

class Lock {
public:
    explicit Lock(const Path& directory);
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_{-1};
};

bool ensure(const Path& directory);
bool sync(const Path& directory);
std::optional<std::uint64_t> size(const Path& path);
Result<Bytes> read(const Path& path, std::uint64_t limit = metadata_limit);
Result<Bytes> slice(const Path& path, Range range, std::uint64_t limit);
bool stream(const Path& path, const Sink& consumer);
std::optional<std::string> digest(const Path& path);
// The setup directory must already exist. The caller serializes writes to
// the same name. A false return can leave a visible but unconfirmed result.
bool publish(const Path& path, const Producer& producer);
bool publish(const Path& path, std::span<const std::byte> bytes);

}  // namespace waste::m09::disk
