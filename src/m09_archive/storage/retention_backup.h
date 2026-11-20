#pragma once

#include "../archive_service.h"

namespace waste::m09 {

struct RetentionState {
    bool unresolved_commands{}, pinned_readers{}, held_for_review{}, retention_period_elapsed{};
};

bool eligible_for_retention_cleanup(const Manifest& manifest, const RetentionState& state);

class RetentionBackup {
public:
    explicit RetentionBackup(Authorize authorize) : authorize_(std::move(authorize)) {}
    // Copy precisely the pinned version into an independent archive root.
    Status copy(std::string_view subject, const ArchiveService& source, ArchiveService& destination,
                std::string_view run, std::uint64_t version) const;

private:
    Authorize authorize_;
};

}  // namespace waste::m09
