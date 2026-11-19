#pragma once

#include "hmi_ports.h"
#include "access_policy.h"

namespace waste::m08 {

template<class T>
struct Response {
    Issue issue{Issue::None};
    std::optional<T> data;
};

class EvidenceService {
public:
    EvidenceService(const Settings& settings, IdentityPort& identity, EvidencePort& evidence)
        : settings_(settings), identity_(identity), evidence_(evidence) {}

    Response<EvidencePage> list(std::string_view credential, const EvidenceQuery& query);
    Response<ExportJob> enqueue(std::string_view credential, const ExportRequest& request);
    Response<ExportJob> status(std::string_view credential, const Scope& scope, std::string_view id);
    Response<std::string> download(std::string_view credential, const Scope& scope, std::string_view id);

private:
    std::optional<Principal> authorize(std::string_view credential, const Scope& scope, bool exporting);

    const Settings& settings_;
    IdentityPort& identity_;
    EvidencePort& evidence_;
};

}  // namespace waste::m08
