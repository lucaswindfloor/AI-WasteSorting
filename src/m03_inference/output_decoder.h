#pragma once

#include "inference_backend.h"

namespace waste::m03 {

std::string validate_output(const Manifest& manifest, const BackendCompletion& output);

}  // namespace waste::m03
