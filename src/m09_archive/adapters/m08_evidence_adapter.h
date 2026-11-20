#pragma once

#include "../evidence/export_service.h"
#include "m08_hmi/hmi_ports.h"

namespace waste::m09 {

class M08EvidenceAdapter final : public m08::EvidencePort {
public:
    M08EvidenceAdapter(const ArchiveService& archive, ExportService& exports, Authorize authorize,
                       m08::Scope scope, std::uint64_t csv_bytes = 8 * 1024 * 1024);

    std::optional<m08::EvidencePage> list(const m08::EvidenceQuery& query, std::string_view reader) override;
    std::optional<m08::ExportJob> enqueue(const m08::ExportRequest& request, std::string_view owner) override;
    std::optional<m08::ExportJob> find(const m08::Scope& scope, std::string_view id, std::string_view owner) override;
    std::optional<std::string> download(const m08::ExportJob& job) override;

private:
    const ArchiveService& archive_;
    ExportService& exports_;
    Authorize authorize_;
    m08::Scope scope_;
    std::uint64_t csv_bytes_;

    m08::ExportJob project(const ExportJob& job) const;
};

}  // namespace waste::m09
