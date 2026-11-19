#include "hmi_service.h"

#include <utility>

namespace waste::m08 {

HmiService::HmiService(Settings settings, IdentityPort& identity, WorkCellPort& cell,
                      JournalPort& store, EvidencePort& evidence, Clock clock)
    : operations_(std::move(settings), identity, cell, store, std::move(clock)),
      evidence_(operations_.settings(), identity, evidence) {}

bool HmiService::recover() { return operations_.recover(); }
void HmiService::advance() { operations_.advance(); }

}  // namespace waste::m08
