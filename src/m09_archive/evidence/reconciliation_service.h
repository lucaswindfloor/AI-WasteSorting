#pragma once

#include "evidence_query.h"

namespace waste::m09 {

RecordKey pass_key(std::string_view scope, std::string_view pass);
RecordKey measurement_key(std::string_view scope, std::string_view pass, std::string_view id);

struct BatchRow {
    std::uint64_t ordinal{};
    std::string pass, target, planned, actual, evidence;
};

std::vector<BatchRow> batch_rows(const ArchiveService& archive, const Manifest& manifest,
                                 Range range, std::uint64_t limit);

class ReconciliationService {
public:
    ReconciliationService(ArchiveService& archive, Authorize authorize)
        : archive_(archive), authorize_(std::move(authorize)) {}
    Result<Manifest> register_pass(std::string_view subject, const MaterialPass& pass);
    Result<Manifest> measure(std::string_view subject, const Measurement& measurement);
    Result<Manifest> revise(std::string_view subject, const Reconciliation& revision);

private:
    ArchiveService& archive_;
    Authorize authorize_;
    bool allowed(std::string_view subject, std::string_view run, std::string_view purpose) const;
};

}  // namespace waste::m09
