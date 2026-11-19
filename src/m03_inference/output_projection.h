#pragma once

#include "inference_backend.h"

namespace waste::m03 {

std::vector<Evidence> project_output(const Manifest& manifest, const PreparedInput& input,
                                     const BackendCompletion& output);

}  // namespace waste::m03
