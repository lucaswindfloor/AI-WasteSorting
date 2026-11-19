#pragma once

#include "operation_service.h"
#include "evidence_service.h"

namespace waste::m08 {

// Serialized API façade. Clients never receive a control or store port.
class HmiService {
public:
    HmiService(Settings settings, IdentityPort& identity, WorkCellPort& cell,
               JournalPort& store, EvidencePort& evidence, Clock clock);
    HmiService(const HmiService&) = delete;
    HmiService& operator=(const HmiService&) = delete;

    bool recover();
    void advance();
    OperationService& operations() { return operations_; }
    EvidenceService& evidence() { return evidence_; }

private:
    OperationService operations_;
    EvidenceService evidence_;
};

}  // namespace waste::m08
