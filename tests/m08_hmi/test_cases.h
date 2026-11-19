#pragma once

#include <stdexcept>

inline void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void access_policy_tests();
void operation_service_tests();
void bin_change_tests();
void configuration_tests();
void projection_and_evidence_tests();
