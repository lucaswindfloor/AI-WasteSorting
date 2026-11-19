#include "evidence_service.h"

#include <algorithm>
#include <utility>

namespace waste::m08 {
namespace {

bool field(std::string_view value, bool required = true) {
    return (!required || !value.empty()) && value.size() <= 128 &&
           std::ranges::all_of(value, [](unsigned char byte) { return byte >= 32 && byte != 127; });
}

}  // namespace

std::optional<Principal> EvidenceService::authorize(std::string_view credential, const Scope& scope,
                                                     bool exporting) {
    if (scope != settings_.scope) {
        return {};
    }
    auto principal = identity_.session(credential);
    if (!principal || !allows(*principal, scope, Permission::Evidence) ||
        (exporting && !allows(*principal, scope, Permission::Export))) {
        return {};
    }
    return principal;
}

Response<EvidencePage> EvidenceService::list(std::string_view credential, const EvidenceQuery& q) {
    const auto principal = authorize(credential, q.scope, false);
    if (!principal) {
        return {Issue::Unauthorized, {}};
    }
    if (!field(q.manifest) || !field(q.cursor, false) || !q.limit || q.limit > settings_.max_page) {
        return {Issue::Invalid, {}};
    }
    auto page = evidence_.list(q, principal->subject);
    if (!page || page->scope != q.scope || page->manifest != q.manifest || page->rows.size() > q.limit) {
        return {Issue::NotFound, {}};
    }
    return {Issue::None, std::move(page)};
}

Response<ExportJob> EvidenceService::enqueue(std::string_view credential, const ExportRequest& r) {
    const auto principal = authorize(credential, r.scope, true);
    if (!principal) {
        return {Issue::Unauthorized, {}};
    }
    if (!field(r.id) || !field(r.manifest) || !field(r.first) || !field(r.last) || !field(r.purpose) ||
        !r.max_rows || r.max_rows > settings_.max_export_rows) {
        return {Issue::Invalid, {}};
    }
    auto job = evidence_.enqueue(r, principal->subject);
    if (!job || job->owner != principal->subject || job->request != r) {
        return {Issue::Conflict, {}};
    }
    return {Issue::None, std::move(job)};
}

Response<ExportJob> EvidenceService::status(std::string_view credential, const Scope& scope,
                                            std::string_view id) {
    const auto principal = authorize(credential, scope, true);
    if (!principal) {
        return {Issue::Unauthorized, {}};
    }
    if (!field(id)) {
        return {Issue::Invalid, {}};
    }
    auto job = evidence_.find(scope, id, principal->subject);
    if (!job || job->owner != principal->subject || job->request.scope != scope || job->request.id != id) {
        return {Issue::NotFound, {}};
    }
    return {Issue::None, std::move(job)};
}

Response<std::string> EvidenceService::download(std::string_view credential, const Scope& scope,
                                                std::string_view id) {
    auto job = status(credential, scope, id);
    if (!job.data) {
        return {job.issue, {}};
    }
    if (job.data->stage != ExportStage::Ready || job.data->checksum.empty()) {
        return {Issue::NotReady, {}};
    }
    auto content = evidence_.download(*job.data);
    return content ? Response<std::string>{Issue::None, std::move(content)}
                   : Response<std::string>{Issue::NotFound, {}};
}

}  // namespace waste::m08
