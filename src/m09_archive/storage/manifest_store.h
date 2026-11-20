#pragma once

#include "local_files.h"

namespace waste::m09 {

class ManifestStore {
public:
    ManifestStore(disk::Path root, std::string run) : root_(std::move(root)), run_(std::move(run)) {}
    Result<Manifest> load(std::uint64_t version = 0) const;
    Result<std::vector<std::uint64_t>> versions() const;
    Status commit(const Manifest& manifest) const;
    disk::Path file(std::uint64_t version) const;

private:
    disk::Path root_;
    std::string run_;
};

}  // namespace waste::m09
