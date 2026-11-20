#pragma once

#include "evidence_query.h"

namespace waste::m09 {

class ExportService {
public:
    ExportService(const ArchiveService& archive, Authorize authorize,
                  std::uint64_t max_bytes = 1024ULL * 1024 * 1024)
        : archive_(archive), authorize_(std::move(authorize)), max_bytes_(max_bytes) {}

    Result<ExportJob> enqueue(const ExportRequest& request);
    Result<ExportJob> status(std::string_view owner, std::string_view run, std::string_view id) const;
    Result<ExportJob> advance(std::string_view owner, std::string_view run, std::string_view id);
    Status stream(std::string_view owner, std::string_view run, std::string_view id, const Sink& sink) const;

private:
    const ArchiveService& archive_;
    Authorize authorize_;
    std::uint64_t max_bytes_;

    disk::Path directory(std::string_view owner, std::string_view run, std::string_view id) const;
    Result<ExportJob> read_job(const disk::Path& path) const;
    std::optional<Selection> approval(const ExportRequest& request) const;
    Status generate(const Manifest& manifest, const ExportJob& job, const disk::Path& output) const;
};

}  // namespace waste::m09
