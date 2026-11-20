#include "local_files.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <openssl/evp.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace waste::m09::disk {
namespace {

class File {
public:
    explicit File(int fd) : fd(fd) {}
    ~File() { if (fd >= 0) ::close(fd); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    int fd;
};

template<class Operation>
auto interrupted(Operation call) {
    decltype(call()) result;
    do { result = call(); } while (result < 0 && errno == EINTR);
    return result;
}

bool writable(int fd, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        constexpr std::size_t chunk = 1024 * 1024;
        const auto length = std::min(chunk, bytes.size());
        const auto written = interrupted([&] { return ::write(fd, bytes.data(), length); });
        if (written <= 0) return false;
        bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
    return true;
}

Result<Bytes> from_file(int fd, Range range, std::uint64_t limit) {
    struct stat info{};
    if (::fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0) return {Status::Unavailable, {}};
    if (range.begin > range.end || range.end > static_cast<std::uint64_t>(info.st_size)) {
        return {Status::Corrupt, {}};
    }
    if (range.end - range.begin > std::min<std::uint64_t>(limit, std::numeric_limits<std::size_t>::max())) {
        return {Status::Capacity, {}};
    }
    Bytes bytes(static_cast<std::size_t>(range.end - range.begin));
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto length = std::min<std::size_t>(1024 * 1024, bytes.size() - offset);
        const auto count = interrupted([&] {
            return ::pread(fd, bytes.data() + offset, length, static_cast<off_t>(range.begin + offset));
        });
        if (count <= 0) return {Status::Corrupt, {}};
        offset += static_cast<std::size_t>(count);
    }
    return {Status::Ok, std::move(bytes)};
}

}  // namespace

Lock::Lock(const Path& directory) {
    const auto file = directory / ".lock";
    fd_ = ::open(file.c_str(), O_RDWR | O_CLOEXEC | O_CREAT | O_NOFOLLOW, 0600);
    if (fd_ >= 0 && interrupted([&] { return ::flock(fd_, LOCK_EX); }) != 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Lock::~Lock() { if (fd_ >= 0) ::close(fd_); }

bool sync(const Path& directory) {
    const File file(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    return file.fd >= 0 && interrupted([&] { return ::fsync(file.fd); }) == 0;
}

bool ensure(const Path& directory) {
    if (directory.empty()) return false;
    std::error_code error;
    const auto parent = directory.has_parent_path() ? directory.parent_path() : Path(".");
    if (std::filesystem::is_directory(std::filesystem::symlink_status(directory, error))) {
        return directory == parent || sync(parent);
    }
    if (parent == directory || !ensure(parent)) return false;
    const bool created = std::filesystem::create_directory(directory, error);
    if (!created && !std::filesystem::is_directory(std::filesystem::symlink_status(directory, error))) {
        return false;
    }
    return sync(parent);
}

std::optional<std::uint64_t> size(const Path& path) {
    struct stat info{};
    if (::lstat(path.c_str(), &info) || !S_ISREG(info.st_mode) || info.st_size < 0) return {};
    return static_cast<std::uint64_t>(info.st_size);
}

Result<Bytes> read(const Path& path, std::uint64_t limit) {
    const File file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.fd < 0) return {errno == ENOENT ? Status::Missing : Status::Unavailable, {}};
    struct stat info{};
    if (::fstat(file.fd, &info) || info.st_size < 0) return {Status::Unavailable, {}};
    return from_file(file.fd, {0, static_cast<std::uint64_t>(info.st_size)}, limit);
}

Result<Bytes> slice(const Path& path, Range range, std::uint64_t limit) {
    const File file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.fd < 0) return {errno == ENOENT ? Status::Missing : Status::Unavailable, {}};
    return from_file(file.fd, range, limit);
}

bool stream(const Path& path, const Sink& consumer) {
    const File file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.fd < 0) return false;
    struct stat info{};
    if (::fstat(file.fd, &info) || !S_ISREG(info.st_mode)) return false;
    std::array<std::byte, 64 * 1024> buffer{};
    while (true) {
        const auto count = interrupted([&] { return ::read(file.fd, buffer.data(), buffer.size()); });
        if (count == 0) return true;
        if (count < 0 || !consumer(std::span(buffer).first(static_cast<std::size_t>(count)))) return false;
    }
}

std::optional<std::string> digest(const Path& path) {
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        !stream(path, [&](auto bytes) {
            return EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) == 1;
        })) return {};
    unsigned char bytes[EVP_MAX_MD_SIZE]{};
    unsigned int length{};
    if (EVP_DigestFinal_ex(context.get(), bytes, &length) != 1 || length != 32) return {};
    std::string hex;
    constexpr std::string_view alphabet = "0123456789abcdef";
    for (unsigned i = 0; i < length; ++i) {
        hex += alphabet[bytes[i] >> 4];
        hex += alphabet[bytes[i] & 15];
    }
    return hex;
}

bool publish(const Path& path, const Producer& producer) {
    auto pending = path;
    pending += ".pending";
    if (::unlink(pending.c_str()) != 0 && errno != ENOENT) return false;
    const File output(::open(pending.c_str(), O_WRONLY | O_CLOEXEC | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
    if (output.fd < 0) return false;
    if (!producer([&](auto bytes) { return writable(output.fd, bytes); })) return false;
    return interrupted([&] { return ::fdatasync(output.fd); }) == 0 &&
           ::rename(pending.c_str(), path.c_str()) == 0 && sync(path.parent_path());
}

bool publish(const Path& path, std::span<const std::byte> bytes) {
    return publish(path, [&](const Sink& write) { return write(bytes); });
}

}  // namespace waste::m09::disk
