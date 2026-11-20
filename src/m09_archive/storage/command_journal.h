#pragma once

#include "local_files.h"

namespace waste::m09 {

struct Checkpoint {
    std::uint64_t revision{};
    std::string transaction;
    Bytes state;
    std::map<std::string, std::string> intent_digests{};
    bool operator==(const Checkpoint&) const = default;
};

// An independent journal directory per station/run and writer type.
class CommandJournal {
public:
    explicit CommandJournal(disk::Path directory) : root_(std::move(directory)) {}
    Result<Checkpoint> load(std::uint64_t revision = 0) const;
    Status commit(std::uint64_t expected, const Checkpoint& next) const;

private:
    disk::Path root_;
    Result<std::uint64_t> head() const;
    Result<Checkpoint> load_at(std::uint64_t revision) const;
    disk::Path checkpoint_path(std::uint64_t revision) const;
};

}  // namespace waste::m09
