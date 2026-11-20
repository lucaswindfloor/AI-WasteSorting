#pragma once

#include "local_files.h"

namespace waste::m09 {

// The caller serializes against producers for this run.
Result<Manifest> recover_archive(const disk::Path& path, const std::string& run,
                                 std::uint64_t max_segment_bytes, std::uint64_t max_entries);

}  // namespace waste::m09
