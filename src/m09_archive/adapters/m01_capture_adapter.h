#pragma once

#include "../archive_service.h"
#include "m01_acquisition/acquisition_types.h"

namespace waste::m09 {

std::string capture_trigger_id(const m01::TriggerKey& key);

class M01CaptureAdapter {
public:
    M01CaptureAdapter(ArchiveService& archive, std::string receipt_domain);
    Result<Manifest> ingest(const m01::Segment& segment);
    Result<Manifest> ingest(const m01::Output& output);

private:
    ArchiveService& archive_;
    std::string receipt_domain_;

    Result<Manifest> diagnose(const m01::Config& config, const std::vector<m01::Diagnostic>& diagnostics);
};

}  // namespace waste::m09
